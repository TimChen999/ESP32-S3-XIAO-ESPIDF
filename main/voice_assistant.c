/*
 * ============================================================================
 * VOICE ASSISTANT — Application Layer
 * ============================================================================
 *
 * This module connects the speaker driver to the backend. It calls
 * speaker_play_url() which opens an HTTP connection to the backend,
 * waits (blocking) for the backend to process and stream PCM audio,
 * then plays it through the speaker. The voice assistant doesn't need
 * to "know" when audio is ready — the HTTP connection stays open while
 * the backend works, and data flows automatically when ready.
 *
 * SYSTEM TASK MAP:
 *
 *   ┌──────────────────────┐
 *   │ voice_assistant_task │ priority 5 — calls speaker_play_url(),
 *   │ (this file)          │              polls state, loops
 *   └──────────┬───────────┘
 *              │ speaker_play_url(url) — non-blocking, returns immediately
 *              ▼ internally spawns:
 *   ┌──────────────────────┐
 *   │ speaker_network_task │ priority 5 — HTTP POST to backend,
 *   │ (speaker_driver.c)   │              blocked on recv() until backend
 *   │ (ephemeral)          │              streams PCM, fills buffer
 *   └──────────┬───────────┘
 *              │ xStreamBufferSend
 *              ▼
 *   ┌──────────────────────┐
 *   │ speaker_playback_    │ priority 6 — reads buffer, volume scales,
 *   │ task                 │              writes I2S. Highest priority:
 *   │ (speaker_driver.c)   │              never starves.
 *   │ (permanent)          │
 *   └──────────────────────┘
 *
 * INTERFACE WITH SPEAKER DRIVER:
 *   This module only uses functions declared in speaker_driver.h:
 *     - speaker_play_url()   → start playback from a URL
 *     - speaker_get_state()  → poll whether playback is done
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

void voice_assistant_init(void)
{
    ESP_LOGI(TAG, "Voice assistant initialized");
}

// ============================================================================
//  VOICE ASSISTANT TASK
//
//  Connects the speaker driver to the backend in a loop:
//    1. Wait for network
//    2. Call speaker_play_url() — opens HTTP to backend, backend processes
//       (LLM/TTS), streams PCM back, speaker plays it
//    3. Wait for playback to finish
//    4. Loop back to step 2
//
//  The call to speaker_play_url() is non-blocking — it returns immediately
//  after spawning the internal network task. The network task holds the
//  HTTP connection open, blocking on recv() until the backend sends data.
//  No trigger mechanism needed: the backend decides when audio is ready.
// ============================================================================
void voice_assistant_task(void *param)
{
    (void)param;

    ESP_LOGI(TAG, "Voice assistant task started");

    // Step 1: Wait for network connectivity (Wi-Fi or PPP)
    // Blocks until IP is acquired. Speaker playback task is already
    // running (created at boot) but sleeping — nothing to play yet.
    network_app_wait_for_connection();
    ESP_LOGI(TAG, "Network ready — voice assistant active");

    while (1) {
        // Step 2: Request audio from backend via speaker driver
        //
        // speaker_play_url() does the following internally:
        //   a. Resets the stream buffer
        //   b. Sets state = BUFFERING
        //   c. Spawns speaker_network_task which opens HTTP POST
        //   d. Returns immediately
        //
        // The network task then holds the connection open. The backend
        // processes the request (LLM generates text, TTS converts to
        // audio, transcodes to PCM) — this may take seconds. The
        // network task is blocked on recv() the entire time (0% CPU).
        // When the backend starts streaming PCM chunks, the network
        // task wakes up and writes them into the stream buffer.
        // When the buffer crosses the prebuffer threshold, the
        // playback task wakes up and starts writing to I2S.
        ESP_LOGI(TAG, "Requesting audio from backend: %s", BACKEND_AUDIO_URL);
        speaker_play_url(BACKEND_AUDIO_URL);

        // Step 3: Wait for playback to finish
        //
        // Poll speaker_get_state() until the driver returns to IDLE
        // (normal completion) or ERROR. This is not a busy wait — each
        // iteration sleeps 200ms. The scheduler runs the network and
        // playback tasks during our sleep.
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

        // Step 4: Loop back to step 2
        // The next speaker_play_url() opens a new HTTP connection.
        // The backend can return different audio each time.
    }
}
