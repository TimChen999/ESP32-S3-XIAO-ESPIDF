/*
 * ============================================================================
 * VOICE ASSISTANT — Application Layer
 * ============================================================================
 *
 * This module is the conversation coordinator. It manages the push-to-talk
 * button, sequences the mic and speaker drivers, and connects both to the
 * backend server for a complete voice conversation loop.
 *
 * Push-to-talk flow:
 *   1. User presses button     → mic_start()   → I2S RX captures audio
 *   2. User releases button    → mic_stop()    → recording ends
 *   3. Upload recorded audio   → mic_upload()  → HTTP POST to backend
 *   4. Backend processes        STT → LLM → TTS → PCM response
 *   5. Play response           → speaker_play_url() → I2S TX plays audio
 *   6. Wait for completion     → loop back to step 1
 *
 * SYSTEM TASK MAP:
 *
 *   ┌──────────────────────────┐
 *   │ voice_assistant_task     │ pri 5 — button poll, sequences mic + speaker
 *   │ (this file)              │
 *   └──────┬──────────┬────────┘
 *          │          │ public API calls
 *          ▼          ▼
 *   ┌──────────┐ ┌──────────────┐
 *   │ mic_drv  │ │ speaker_drv  │
 *   │          │ │              │
 *   │ mic_     │ │ spk_network_ │ pri 5 — ephemeral tasks (upload / fetch)
 *   │ upload   │ │ task         │
 *   │ (ephem.) │ │ (ephemeral)  │
 *   │          │ │              │
 *   │ mic_     │ │ spk_playback │ pri 6 — permanent tasks (I2S hardware)
 *   │ capture  │ │ _task        │
 *   │ (perm.)  │ │ (permanent)  │
 *   └──────────┘ └──────────────┘
 *
 * BUTTON HANDLING:
 *   The push-to-talk button is handled here, NOT in the mic driver.
 *   This keeps the mic driver hardware-agnostic — it just exposes
 *   mic_start() and mic_stop(). The trigger mechanism (button, VAD,
 *   fixed-duration, etc.) can be changed in this file without touching
 *   the mic driver at all.
 *
 *   To swap from push-to-talk to voice activity detection (VAD):
 *     1. Replace button_pressed() with a VAD function that analyzes
 *        mic samples for speech presence
 *     2. Change the polling loop to start/stop based on VAD output
 *     3. No changes to mic_driver.c needed
 *
 * ============================================================================
 */

#include "voice_assistant.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "board_config.h"
#include "network_app.h"
#include "speaker_driver.h"
#include "mic_driver.h"

// ============================================================================
//  MIC INTEGRATION TOGGLE
//
//  When MIC_ENABLED is 1, the voice assistant uses push-to-talk:
//    button press → record → upload → play response → repeat
//
//  When MIC_ENABLED is 0, the voice assistant uses the original behavior:
//    play_url → wait → repeat (no mic, no button — speaker only)
//
//  Set to 0 to test the speaker driver in isolation.
// ============================================================================
#define MIC_ENABLED             1

// ============================================================================
//  PUSH-TO-TALK BUTTON CONFIGURATION
//
//  PTT_BUTTON_ENABLED = 1:
//    Uses a physical GPIO button for push-to-talk. Hold to record,
//    release to stop. Wire a momentary switch between the pin and GND.
//    Internal pull-up keeps the pin HIGH when not pressed.
//
//  PTT_BUTTON_ENABLED = 0:
//    Auto-record mode for testing without a physical button. Records
//    for PTT_AUTO_RECORD_MS milliseconds, then uploads and plays.
//    Useful for Wokwi simulation or bench testing.
//
//  PTT_BUTTON_PIN is set in board_config.h.
// ============================================================================
#define PTT_BUTTON_ENABLED      0
#define PTT_AUTO_RECORD_MS      3000

#if PTT_BUTTON_ENABLED && MIC_ENABLED
#include "driver/gpio.h"
#endif

static const char *TAG = "VOICE_ASST";

// ============================================================================
//  BACKEND CONFIGURATION
//
//  BACKEND_MIC_URL and BACKEND_SPEAKER_URL are set in board_config.h.
//  The fallbacks below apply only if board_config.h doesn't define them.
// ============================================================================
#ifndef BACKEND_MIC_URL
#define BACKEND_MIC_URL         "http://your-backend.example.com/api/listen"
#endif
#ifndef BACKEND_SPEAKER_URL
#define BACKEND_SPEAKER_URL     "http://your-backend.example.com/api/speak"
#endif

// ============================================================================
//  BUTTON HELPERS
//
//  Simple GPIO polling for push-to-talk. The 20ms poll interval provides
//  basic debouncing — a brief contact bounce (<20ms) is filtered naturally.
//
//  To upgrade to GPIO interrupt-driven detection:
//    1. Configure the pin with GPIO_INTR_NEGEDGE (press) / POSEDGE (release)
//    2. Use a semaphore or task notification instead of polling
//    3. Keep the same mic_start()/mic_stop() calls
// ============================================================================
#if PTT_BUTTON_ENABLED && MIC_ENABLED
static bool button_pressed(void)
{
    return gpio_get_level(PTT_BUTTON_PIN) == 0;
}
#endif

void voice_assistant_init(void)
{
#if PTT_BUTTON_ENABLED && MIC_ENABLED
    gpio_config_t io_conf = {
        .pin_bit_mask  = (1ULL << PTT_BUTTON_PIN),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_LOGI(TAG, "Push-to-talk button configured on GPIO%d", PTT_BUTTON_PIN);
#endif

    ESP_LOGI(TAG, "Voice assistant initialized (MIC_ENABLED=%d, PTT_BUTTON=%d)",
             MIC_ENABLED, PTT_BUTTON_ENABLED);
}

// ============================================================================
//  VOICE ASSISTANT TASK
//
//  Main conversation loop. After network is ready, enters one of two modes:
//
//  MIC_ENABLED=1 (push-to-talk):
//    1. Wait for button press (or auto-record if PTT_BUTTON_ENABLED=0)
//    2. Record mic audio
//    3. Upload to backend
//    4. Play backend's audio response
//    5. Loop
//
//  MIC_ENABLED=0 (speaker-only):
//    1. Call speaker_play_url() with the backend URL
//    2. Wait for playback to finish
//    3. Loop
//
//  Interaction sequence (MIC_ENABLED=1):
//
//    User        voice_assistant    mic_driver       speaker_driver   backend
//    ────        ───────────────    ──────────       ──────────────   ───────
//    press ─────► detected
//                 ├─ mic_start() ──► I2S RX on
//    hold         │                  recording...
//    release ────► detected
//                 ├─ mic_stop() ───► I2S RX off
//                 ├─ mic_upload() ─► HTTP POST ─────────────────────► recv
//                 │                  ...                                │ STT
//                 │   IDLE ◄──────── done                               │ LLM
//                 │                                                     │ TTS
//                 ├─ speaker_play_url() ──────► HTTP POST ────────────► │
//                 │                             recv()                  │
//                 │                             ◄──── [PCM chunks]
//                 │                             buffer → I2S → speaker
//                 │   IDLE ◄────────────────── done
//                 └── loop
// ============================================================================
void voice_assistant_task(void *param)
{
    (void)param;

    ESP_LOGI(TAG, "Voice assistant task started");

    network_app_wait_for_connection();
    ESP_LOGI(TAG, "Network ready — voice assistant active");

#if MIC_ENABLED
    // -----------------------------------------------------------------------
    //  Push-to-talk conversation loop
    // -----------------------------------------------------------------------
    while (1) {
        // Step 1: Wait for recording trigger
#if PTT_BUTTON_ENABLED
        ESP_LOGI(TAG, "Waiting for button press (GPIO%d)...", PTT_BUTTON_PIN);
        while (!button_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        ESP_LOGI(TAG, "Button pressed — starting recording");
#else
        ESP_LOGI(TAG, "Auto-record: starting in 1 second...");
        vTaskDelay(pdMS_TO_TICKS(1000));
#endif

        // Step 2: Start recording
        mic_start();

        // Step 3: Wait for recording end (button release or fixed duration)
#if PTT_BUTTON_ENABLED
        while (button_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        ESP_LOGI(TAG, "Button released — stopping recording");
#else
        ESP_LOGI(TAG, "Auto-recording for %d ms...", PTT_AUTO_RECORD_MS);
        vTaskDelay(pdMS_TO_TICKS(PTT_AUTO_RECORD_MS));
#endif

        // Step 4: Stop recording — audio data stays in stream buffer
        mic_stop();

        // Step 5: Upload captured audio to backend
        ESP_LOGI(TAG, "Uploading audio to: %s", BACKEND_MIC_URL);
        mic_upload(BACKEND_MIC_URL);

        // Step 6: Wait for upload to complete
        while (1) {
            mic_state_t mic_state = mic_get_state();

            if (mic_state == MIC_STATE_IDLE || mic_state == MIC_STATE_DONE) {
                ESP_LOGI(TAG, "Upload complete");
                break;
            }
            if (mic_state == MIC_STATE_ERROR) {
                ESP_LOGE(TAG, "Upload error — skipping playback");
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (mic_get_state() == MIC_STATE_ERROR) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Step 7: Play backend's audio response
        ESP_LOGI(TAG, "Playing response from: %s", BACKEND_SPEAKER_URL);
        speaker_play_url(BACKEND_SPEAKER_URL);

        // Step 8: Wait for playback to finish
        while (1) {
            speaker_state_t spk_state = speaker_get_state();

            if (spk_state == SPEAKER_STATE_IDLE) {
                ESP_LOGI(TAG, "Playback complete");
                break;
            }
            if (spk_state == SPEAKER_STATE_ERROR) {
                ESP_LOGE(TAG, "Playback error — check backend and network");
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(200));
        }

        // Step 9: Brief pause before next interaction
        vTaskDelay(pdMS_TO_TICKS(200));
    }

#else
    // -----------------------------------------------------------------------
    //  Speaker-only loop (original behavior, no mic)
    // -----------------------------------------------------------------------
    while (1) {
        ESP_LOGI(TAG, "Requesting audio from backend: %s", BACKEND_SPEAKER_URL);
        speaker_play_url(BACKEND_SPEAKER_URL);

        while (1) {
            speaker_state_t state = speaker_get_state();

            if (state == SPEAKER_STATE_IDLE) {
                ESP_LOGI(TAG, "Playback complete");
                break;
            }
            if (state == SPEAKER_STATE_ERROR) {
                ESP_LOGE(TAG, "Playback error — check backend URL and network");
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
#endif
}