/*
 * ============================================================================
 * VOICE ASSISTANT — Application Layer
 * ============================================================================
 *
 * This module is the APPLICATION LAYER that sits above the speaker driver.
 * It does not own any hardware or manage any buffers — it only decides
 * WHEN to trigger audio playback and calls the speaker driver's public API.
 *
 * ┌────────────────────────────────────────────────────────────────────────┐
 * │                       SYSTEM TASK MAP                                 │
 * │                                                                        │
 * │  ┌──────────────────────┐                                             │
 * │  │ voice_assistant_task │ priority 5   APPLICATION — triggers playback │
 * │  │ (this file)          │              calls speaker_play_url()        │
 * │  └──────────┬───────────┘              polls speaker_get_state()      │
 * │             │                                                          │
 * │             │ speaker_play_url(url) — non-blocking, returns immediately│
 * │             │                                                          │
 * │             ▼ internally spawns:                                       │
 * │  ┌──────────────────────┐                                             │
 * │  │ speaker_network_task │ priority 5   DRIVER — fetches PCM over HTTP │
 * │  │ (speaker_driver.c)   │              writes to stream buffer         │
 * │  │ (ephemeral)          │              self-deletes when done          │
 * │  └──────────┬───────────┘                                             │
 * │             │ xStreamBufferSend                                        │
 * │             ▼                                                          │
 * │  ┌──────────────────────┐                                             │
 * │  │ speaker_playback_    │ priority 6   DRIVER — reads buffer → I2S    │
 * │  │ task                 │              highest priority: I2S never     │
 * │  │ (speaker_driver.c)   │              starves even if network blocks  │
 * │  │ (permanent)          │                                             │
 * │  └──────────────────────┘                                             │
 * │                                                                        │
 * │  This task (voice_assistant) and the network task both run at          │
 * │  priority 5. They never compete because this task is blocked in       │
 * │  vTaskDelay (polling speaker state) while the network task is         │
 * │  blocked in esp_http_client_read (waiting for backend data).          │
 * │  The playback task at priority 6 preempts both whenever the stream    │
 * │  buffer has data to consume.                                          │
 * └────────────────────────────────────────────────────────────────────────┘
 *
 * INTERFACE WITH SPEAKER DRIVER:
 *   This module only uses functions declared in speaker_driver.h:
 *     - speaker_play_url()   → start playback from a URL
 *     - speaker_get_state()  → poll whether playback is done
 *     - speaker_stop()       → cancel playback (not used yet)
 *     - speaker_set_volume() → adjust volume (not used yet)
 *   No internal driver state, buffers, or I2S handles are touched.
 *
 * ============================================================================
 */

#include "voice_assistant.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "network_app.h"
#include "speaker_driver.h"

static const char *TAG = "VOICE_ASST";

// ============================================================================
//  BACKEND CONFIGURATION
//
//  URL of the backend endpoint that returns raw PCM audio.
//  The backend receives the HTTP POST and responds with:
//    Content-Type: application/octet-stream
//    Transfer-Encoding: chunked
//    Body: raw PCM bytes (s16le, matching speaker driver's AUDIO_PRESET)
//
//  Change this to point at your actual backend server.
// ============================================================================
#define BACKEND_AUDIO_URL   "http://your-backend.example.com/api/speak"

// Delay between playback sessions (seconds). In a real product this
// would be replaced by a trigger mechanism (button, wake word, etc.).
#define TRIGGER_INTERVAL_S  10

void voice_assistant_init(void)
{
    ESP_LOGI(TAG, "Voice assistant initialized");
}

// ============================================================================
//  VOICE ASSISTANT TASK
//
//  This is the entry point for the application-layer audio loop.
//  It runs forever, cycling through: wait → trigger → play → wait.
//
//  Step-by-step interaction with the speaker driver:
//
//    ┌──────────────────────────────────────────────────────────────────┐
//    │  voice_assistant_task          speaker driver internals          │
//    │  ─────────────────────         ────────────────────────          │
//    │                                                                  │
//    │  Step 1: wait for network      (speaker_playback_task sleeping)  │
//    │          ↓                                                       │
//    │  Step 2: wait for trigger      (playback task still sleeping)    │
//    │          ↓                                                       │
//    │  Step 3: speaker_play_url() ──► spawns network task              │
//    │          returns immediately    state → BUFFERING                │
//    │          ↓                      network task: HTTP connect...    │
//    │  Step 4: poll get_state()       network task: reading chunks...  │
//    │          see BUFFERING          buffer filling up                │
//    │          ↓                      buffer ≥ threshold → PLAYING    │
//    │          see PLAYING            playback task wakes: I2S writes  │
//    │          ↓                      (both tasks running in parallel) │
//    │          see DRAINING           HTTP done, buffer draining       │
//    │          ↓                      buffer empty → STOPPED          │
//    │          see IDLE               playback task: I2S off, sleeping │
//    │          ↓                                                       │
//    │  Step 5: loop back to step 2                                     │
//    └──────────────────────────────────────────────────────────────────┘
// ============================================================================
void voice_assistant_task(void *param)
{
    (void)param;

    ESP_LOGI(TAG, "Voice assistant task started");

    // -----------------------------------------------------------------------
    // Step 1: Wait for network connectivity
    //
    // COMPONENT: network_app (network_app.c)
    // CONCEPTUAL: This task cannot do anything until the ESP32 has an IP
    //   address. network_app_wait_for_connection() blocks (via FreeRTOS
    //   event group) until either Wi-Fi or PPP modem acquires an IP.
    //   After this returns, HTTP requests will work.
    //
    // INTERACTION: No interaction with speaker driver yet. The speaker's
    //   playback task is already running (created at boot) but sleeping
    //   in its Phase 1 loop, polling for PLAYING state every 20ms.
    // -----------------------------------------------------------------------
    network_app_wait_for_connection();
    ESP_LOGI(TAG, "Network ready — voice assistant active");

    while (1) {
        // -------------------------------------------------------------------
        // Step 2: Wait for trigger
        //
        // COMPONENT: application logic (this file)
        // CONCEPTUAL: Something must decide "now is the time to speak."
        //   In a real product this would be:
        //     - GPIO interrupt from a button press
        //     - Wake word detected by mic driver + VAD
        //     - Command received over MQTT/WebSocket
        //     - Timer-based periodic announcement
        //
        //   For now, we auto-trigger after a delay so the pipeline can
        //   be tested end-to-end without external hardware.
        //
        // INTERACTION: Speaker driver is IDLE during this wait. All three
        //   components are sleeping:
        //     - This task: blocked in vTaskDelay
        //     - Playback task: sleeping in Phase 1 poll loop
        //     - Network task: does not exist yet
        //
        // TODO: Replace this delay with actual trigger logic:
        //   - Button: xQueueReceive(button_queue, ..., portMAX_DELAY)
        //   - Wake word: xSemaphoreTake(wake_word_sem, portMAX_DELAY)
        //   - Mic VAD: wait for silence-to-speech transition event
        // -------------------------------------------------------------------
        ESP_LOGI(TAG, "Waiting %d seconds before next audio request...", TRIGGER_INTERVAL_S);
        vTaskDelay(pdMS_TO_TICKS(TRIGGER_INTERVAL_S * 1000));

        // -------------------------------------------------------------------
        // Step 3: Trigger playback via speaker driver
        //
        // COMPONENT: speaker driver public API (speaker_driver.h)
        // CONCEPTUAL: This is the ONE CALL that kicks off the entire audio
        //   pipeline. Everything after this is handled by the driver internally.
        //
        //   What speaker_play_url() does when we call it:
        //     a. Copies the URL to an internal static buffer
        //     b. Resets the stream buffer (clears any old data)
        //     c. Sets state = BUFFERING
        //     d. Spawns speaker_network_task (priority 5, ephemeral)
        //     e. Returns immediately — NON-BLOCKING
        //
        //   What happens next (inside the driver, parallel to us):
        //     - Network task opens HTTP POST to BACKEND_AUDIO_URL
        //     - Backend processes request (LLM/TTS pipeline)
        //     - Network task reads chunked PCM response into stream buffer
        //     - When buffer ≥ prebuffer threshold → state = PLAYING
        //     - Playback task (priority 6) wakes up, reads buffer, writes I2S
        //     - Network and playback tasks run in parallel until HTTP ends
        //     - Network task sets DRAINING, self-deletes
        //     - Playback task drains buffer, sets STOPPED → IDLE
        //
        // INTERACTION: After this call returns, three tasks are active:
        //     1. This task (polling state below)
        //     2. speaker_network_task (filling buffer from HTTP)
        //     3. speaker_playback_task (emptying buffer to I2S)
        //   All communicate through the shared state variable only.
        // -------------------------------------------------------------------
        ESP_LOGI(TAG, "Requesting audio from backend: %s", BACKEND_AUDIO_URL);
        speaker_play_url(BACKEND_AUDIO_URL);

        // -------------------------------------------------------------------
        // Step 4: Wait for playback to complete
        //
        // COMPONENT: speaker driver public API (speaker_get_state)
        // CONCEPTUAL: We poll the driver's state machine to know when
        //   playback finishes. The state transitions we'll see:
        //
        //     BUFFERING → PLAYING → DRAINING → STOPPED → IDLE
        //
        //   We wait until the driver returns to IDLE (normal completion)
        //   or ERROR (something went wrong).
        //
        //   This polling loop is NOT a busy wait — each iteration sleeps
        //   for 200ms via vTaskDelay, using 0% CPU while sleeping. The
        //   scheduler runs the network and playback tasks during our sleep.
        //
        // INTERACTION: While we poll, the driver tasks are doing the
        //   real work:
        //     - Network task: blocked on esp_http_client_read() most of
        //       the time (0% CPU, wakes on incoming TCP data)
        //     - Playback task: blocked on xStreamBufferReceive() or
        //       i2s_channel_write() (0% CPU, wakes on buffer data or
        //       DMA completion)
        //     - This task: blocked on vTaskDelay (0% CPU)
        //   All three tasks are sleeping most of the time. The CPU is
        //   largely idle — work only happens in short bursts when data
        //   arrives from the network or the I2S DMA needs more samples.
        // -------------------------------------------------------------------
        speaker_state_t state;
        while (1) {
            state = speaker_get_state();

            if (state == SPEAKER_STATE_IDLE) {
                ESP_LOGI(TAG, "Playback complete — returned to IDLE");
                break;
            }
            if (state == SPEAKER_STATE_ERROR) {
                ESP_LOGE(TAG, "Playback error — check backend URL and network");
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(200));
        }

        // -------------------------------------------------------------------
        // Step 5: Post-playback handling
        //
        // COMPONENT: application logic (this file)
        // CONCEPTUAL: Playback is done. The driver has returned to IDLE:
        //   - I2S channel is disabled (clocks stopped, amp quiet)
        //   - Stream buffer is empty
        //   - Network task has self-deleted
        //   - Playback task is back to sleeping in its Phase 1 loop
        //
        //   If an error occurred, we log it and continue. In a production
        //   system you might retry, alert the user, or enter a degraded mode.
        //
        // INTERACTION: Speaker driver is fully idle. No resources held.
        //   Safe to call speaker_play_url() again immediately or wait
        //   for the next trigger.
        //
        // TODO: When mic driver is implemented, the full conversation loop
        //   will look like:
        //     1. Wait for wake word (mic driver + VAD)
        //     2. Record mic audio (mic_driver captures to buffer)
        //     3. speaker_play_url() with mic audio in POST body
        //     4. Backend: STT → LLM → TTS → stream PCM back
        //     5. Speaker plays response
        //     6. Loop
        // -------------------------------------------------------------------
        if (state == SPEAKER_STATE_ERROR) {
            ESP_LOGW(TAG, "Retrying after error...");
        }

        // Loop back to Step 2: wait for next trigger
    }
}
