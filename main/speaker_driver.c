#include "speaker_driver.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_http_client.h"

#ifndef SPEAKER_SIMULATE
#define SPEAKER_SIMULATE 0
#endif

#if !SPEAKER_SIMULATE
#include "driver/i2s_std.h"
#endif

static const char *TAG = "SPEAKER_DRV";

// ============================================================================
//  AUDIO FORMAT CONFIGURATION
//
//  Select a preset by changing AUDIO_PRESET. These are compile-time
//  constants — the audio format does not change at runtime.
//
//  AUDIO_PRESET_SPEECH: 16 kHz / 16-bit / mono   (32 KB/s, for LLM/TTS)
//  AUDIO_PRESET_MUSIC:  44.1 kHz / 16-bit / stereo (176 KB/s, CD quality)
//
//  The backend MUST send audio in exactly this format. The ESP32 does no
//  format validation — a mismatch produces noise, not an error.
//
//  Bandwidth at each preset:
//    Speech: 16000 × 2 × 1 = 32,000 B/s   = 256 kbps
//    Music:  44100 × 2 × 2 = 176,400 B/s  = 1.4 Mbps
// ============================================================================
#define AUDIO_PRESET_SPEECH     0
#define AUDIO_PRESET_MUSIC      1
#define AUDIO_PRESET            AUDIO_PRESET_SPEECH

#if AUDIO_PRESET == AUDIO_PRESET_SPEECH
#define SPEAKER_SAMPLE_RATE     16000
#define SPEAKER_BITS            16
#define SPEAKER_CHANNELS        1
#elif AUDIO_PRESET == AUDIO_PRESET_MUSIC
#define SPEAKER_SAMPLE_RATE     44100
#define SPEAKER_BITS            16
#define SPEAKER_CHANNELS        2
#endif

// ============================================================================
//  I2S PIN CONFIGURATION
//
//  Assign GPIO pins for the I2S bus connecting to the amplifier (e.g.
//  MAX98357A, NS4168). No I2C pins are needed for I2S-only amps.
//
//  Default assignment avoids conflict with modem UART pins (D0–D5, D8–D9):
//    BCLK → GPIO9  (D10/SDA on XIAO)
//    WS   → GPIO10 (D7/SCL on XIAO)
//    DOUT → GPIO44 (D6 on XIAO)
//
//  In WiFi-only mode (modem pins free), any GPIO can be used.
//  If using a codec chip with I2C, reassign I2S to D0–D2 and keep
//  D10/D7 for the I2C bus (SDA/SCL).
// ============================================================================
#define I2S_BCLK_PIN            9
#define I2S_WS_PIN              10
#define I2S_DOUT_PIN            44

// ============================================================================
//  RING BUFFER CONFIGURATION
//
//  The stream buffer sits between the network task (producer) and the
//  playback task (consumer). It absorbs network jitter so the speaker
//  keeps playing smoothly even if the network stalls briefly.
//
//  At 16 kHz / 16-bit / mono (32 KB/s):
//    16 KB buffer = 500 ms of audio
//    Pre-buffer at 25% = 125 ms before playback starts
//
//  ESP32-S3 has 512 KB SRAM, so 16 KB is ~3% of available memory.
// ============================================================================
#define RING_BUFFER_SIZE        16384
#define PREBUFFER_THRESHOLD     (RING_BUFFER_SIZE / 4)
#define STREAM_TRIGGER_LEVEL    1

// ============================================================================
//  VOLUME CONFIGURATION — Software Scaling
//
//  Multiplies each 16-bit PCM sample by a float [0.0, 1.0] before
//  writing to I2S. Works with any amplifier, no extra hardware needed.
//  CPU cost: negligible (~1% of one core at 16 kHz).
//
//  Set VOLUME_SCALING_ENABLED to 0 to bypass all volume processing
//  (samples pass through to I2S unmodified).
// ============================================================================
#define VOLUME_SCALING_ENABLED  1
#define DEFAULT_VOLUME          0.8f

// ============================================================================
//  TASK & BUFFER SIZES
// ============================================================================
#define PLAYBACK_BUF_SIZE       1024
#define NET_READ_BUF_SIZE       1024
#define NET_TASK_STACK_SIZE     4096
#define SPEAKER_NET_TASK_PRIORITY  5
#define MAX_URL_LEN             512

// ============================================================================
//  STATIC STATE
// ============================================================================
static speaker_config_t s_speaker_cfg = {
    .bclk_pin          = I2S_BCLK_PIN,
    .ws_pin            = I2S_WS_PIN,
    .dout_pin          = I2S_DOUT_PIN,
    .sample_rate       = SPEAKER_SAMPLE_RATE,
    .bits_per_sample   = SPEAKER_BITS,
    .channels          = SPEAKER_CHANNELS,
    .ring_buffer_size  = RING_BUFFER_SIZE,
    .prebuffer_threshold = PREBUFFER_THRESHOLD,
    .volume            = DEFAULT_VOLUME,
    .state             = SPEAKER_STATE_IDLE,
};

// This allows for the sim to run on wokwi (all the calls are piped to wokwi instead of hardware)
#if !SPEAKER_SIMULATE
static i2s_chan_handle_t s_tx_handle = NULL;
#endif
static StreamBufferHandle_t s_audio_stream = NULL;
static TaskHandle_t s_network_task_handle = NULL;
static bool s_i2s_enabled = false;
static char s_play_url[MAX_URL_LEN];

// ============================================================================
//  VOLUME SCALING
//
//  Scales each 16-bit PCM sample by the volume factor. Called by the
//  playback task on every buffer before writing to I2S. When volume is
//  1.0, the multiplication is effectively a no-op (compiler may optimize).
//
//  To bypass this entirely (e.g. using hardware gain on the amp's GAIN
//  pin instead), set VOLUME_SCALING_ENABLED to 0.
// ============================================================================
#if VOLUME_SCALING_ENABLED
static void apply_volume(int16_t *samples, size_t count, float volume)
{
    for (size_t i = 0; i < count; i++) {
        samples[i] = (int16_t)(samples[i] * volume);
    }
}
#endif

// ============================================================================
//  I2S INITIALIZATION
//
//  Configures the I2S peripheral for standard Philips mode (TX only).
//  This is all that's needed for I2S-only amplifiers:
//    - MAX98357A:  wire BCLK, LRC, DIN + power → plays audio
//    - NS4168:     same pinout, same protocol
//    - PCM5102A:   same pinout, Hi-Fi DAC quality
//
//  Codec chips (ES8311, WM8960) would additionally need:
//    - I2C bus initialization (SDA/SCL pins)
//    - 10–30 I2C register writes for clock config, power sequencing,
//      input/output routing, gain settings
//    - Not supported by this driver.
//
//  I2S initialization sequence:
//    1. i2s_new_channel()          → create TX channel handle
//    2. i2s_channel_init_std_mode() → configure clock, slots, GPIO
//    3. Channel stays disabled until playback starts
// ============================================================================
void speaker_init(void)
{
    speaker_config_t *cfg = &s_speaker_cfg;

#if !SPEAKER_SIMULATE
    // Step 1: Create I2S TX channel (output only — no microphone RX)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_handle, NULL));

    // Step 2: Configure I2S standard (Philips) mode
    //   Clock:  sample rate determines BCLK and WS timing
    //   Slots:  16-bit samples, mono or stereo layout
    //   GPIO:   BCLK, WS, DOUT pin assignments (no MCLK needed for these amps)
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(cfg->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT,
                        (cfg->channels == 1) ? I2S_SLOT_MODE_MONO
                                             : I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)cfg->bclk_pin,
            .ws   = (gpio_num_t)cfg->ws_pin,
            .dout = (gpio_num_t)cfg->dout_pin,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_handle, &std_cfg));

    // I2S channel created and configured but NOT enabled.
    // The playback task enables it when audio data is ready.
#else
    ESP_LOGI(TAG, "SPEAKER_SIMULATE=1 — I2S hardware disabled (logging writes instead)");
#endif

    // Step 3: Allocate stream buffer (ring buffer between network and playback tasks)
    s_audio_stream = xStreamBufferCreate(cfg->ring_buffer_size, STREAM_TRIGGER_LEVEL);
    if (s_audio_stream == NULL) {
        ESP_LOGE(TAG, "Failed to create stream buffer (%d bytes)", (int)cfg->ring_buffer_size);
        cfg->state = SPEAKER_STATE_ERROR;
        return;
    }

    cfg->state = SPEAKER_STATE_IDLE;
    ESP_LOGI(TAG, "speaker_init: rate=%lu bits=%d ch=%d buf=%d pins=BCLK:%d/WS:%d/DOUT:%d",
             (unsigned long)cfg->sample_rate, cfg->bits_per_sample, cfg->channels,
             (int)cfg->ring_buffer_size, cfg->bclk_pin, cfg->ws_pin, cfg->dout_pin);
}

// ============================================================================
//  NETWORK STREAMING TASK (Producer)
//
//  Internal task spawned by speaker_play_url(). Fetches PCM audio over
//  HTTP and feeds it into the stream buffer for the playback task.
//
//  Audio pipeline — conceptual step-by-step:
//
//    Step 1: ESP32 opens HTTP POST to backend URL
//            ┌─────────────────────────────────────────────────────┐
//            │ In the future, the request body carries mic audio:  │
//            │   mic captures → PCM in body → backend receives     │
//            │ For now, request body is empty (no mic yet).        │
//            └─────────────────────────────────────────────────────┘
//
//    Step 2: Backend processes the request
//            ┌─────────────────────────────────────────────────────┐
//            │ Backend receives request (+ future mic audio)       │
//            │   → Speech-to-text on mic audio (future)            │
//            │   → LLM generates response text                     │
//            │   → TTS converts text to audio                      │
//            │   → Transcode to raw PCM (s16le, our sample rate)   │
//            └─────────────────────────────────────────────────────┘
//
//    Step 3: Backend streams PCM response (chunked transfer encoding)
//            ┌─────────────────────────────────────────────────────┐
//            │ HTTP 200 OK                                         │
//            │ Content-Type: application/octet-stream              │
//            │ Transfer-Encoding: chunked                          │
//            │                                                     │
//            │ chunk: [1024 bytes raw PCM]                         │
//            │ chunk: [1024 bytes raw PCM]                         │
//            │ chunk: [512 bytes raw PCM]                          │
//            │ chunk: 0 (end of stream)                            │
//            └─────────────────────────────────────────────────────┘
//
//    Step 4: This task writes each chunk into the stream buffer
//            ┌─────────────────────────────────────────────────────┐
//            │ For each chunk received from HTTP:                   │
//            │   xStreamBufferSend(audio_stream, chunk, len)       │
//            │   (blocks if buffer full → back-pressures network)  │
//            │                                                     │
//            │ When buffer ≥ prebuffer_threshold:                   │
//            │   state = PLAYING → playback task starts consuming  │
//            └─────────────────────────────────────────────────────┘
//
//    Step 5: HTTP transfer complete → state = DRAINING
//            ┌─────────────────────────────────────────────────────┐
//            │ No more data from network.                          │
//            │ Playback task drains remaining buffer → STOPPED.    │
//            └─────────────────────────────────────────────────────┘
// ============================================================================
static void speaker_network_task(void *param)
{
    (void)param;
    speaker_config_t *cfg = &s_speaker_cfg;

    ESP_LOGI(TAG, "Network task started — URL: %s", s_play_url);

    // -----------------------------------------------------------------------
    // Step 1: Configure and open HTTP connection to the backend
    //
    // Uses HTTP POST. The backend expects:
    //   - Request: optionally carries mic audio (TODO) or text prompt
    //   - Response: raw PCM audio stream (chunked transfer encoding)
    //
    // The X-Audio-Format header tells the backend what PCM format the ESP32
    // expects. The backend MUST transcode its TTS output to match.
    // -----------------------------------------------------------------------
    esp_http_client_config_t http_cfg = {
        .url        = s_play_url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = 30000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        cfg->state = SPEAKER_STATE_ERROR;
        goto done;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");

    // Tell the backend what PCM format we need
    char fmt_header[64];
    snprintf(fmt_header, sizeof(fmt_header), "pcm;rate=%lu;bits=%d;channels=%d",
             (unsigned long)cfg->sample_rate, cfg->bits_per_sample, cfg->channels);
    esp_http_client_set_header(client, "X-Audio-Format", fmt_header);

    // -----------------------------------------------------------------------
    // Step 2: Send request to the backend
    //
    // TODO — Mic audio upload:
    //   When the mic driver is implemented, the request body will contain
    //   recorded PCM audio from the microphone. The full round-trip flow:
    //     1. Mic driver captures audio into a buffer
    //     2. This task sends mic PCM as the HTTP POST body
    //     3. Backend receives mic audio → STT → LLM → TTS → PCM response
    //     4. This task reads the PCM response (Steps 3–5 below)
    //   For now, we send a request with no body (content_length = 0).
    //   The backend can return test audio or process a default prompt.
    // -----------------------------------------------------------------------
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        cfg->state = SPEAKER_STATE_ERROR;
        goto cleanup;
    }

    // -----------------------------------------------------------------------
    // Step 3: Read response headers
    //
    // The backend responds with chunked raw PCM:
    //   HTTP 200 OK
    //   Content-Type: application/octet-stream
    //   Transfer-Encoding: chunked (content_length = -1)
    //
    // We don't validate the PCM format — a mismatch produces noise, not
    // errors. Ensure the backend sends s16le at the configured sample rate.
    // -----------------------------------------------------------------------
    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    if (status_code != 200) {
        ESP_LOGE(TAG, "Backend returned HTTP %d (expected 200)", status_code);
        cfg->state = SPEAKER_STATE_ERROR;
        goto cleanup;
    }
    ESP_LOGI(TAG, "HTTP 200 OK — content_length=%d (chunked if -1)", content_length);

    // -----------------------------------------------------------------------
    // Step 4: Stream PCM response body into ring buffer
    //
    // Read chunks of raw PCM bytes from the HTTP response body. Each chunk
    // is written into the stream buffer. The playback task (consumer) reads
    // from the other end and writes to I2S.
    //
    // Back-pressure: when the buffer is full, xStreamBufferSend blocks
    //   until the playback task consumes data. This naturally throttles
    //   the network read rate to match the audio playback rate.
    //
    // Pre-buffer: before starting I2S playback, we wait until the buffer
    //   holds at least prebuffer_threshold bytes. This prevents the first
    //   few hundred ms from being choppy while the network ramps up.
    // -----------------------------------------------------------------------
    {
        char read_buf[NET_READ_BUF_SIZE];
        size_t total_bytes = 0;
        bool prebuffer_done = false;
        int read_len;

        while (1) {
            // Check if we've been asked to stop
            if (cfg->state == SPEAKER_STATE_IDLE) {
                ESP_LOGI(TAG, "Network task: stop requested");
                goto cleanup;
            }

            // Blocking for backend to send audio data to play
            // This is event triggered based on the HTTP client event handler
            read_len = esp_http_client_read(client, read_buf, sizeof(read_buf));

            if (read_len < 0) {
                ESP_LOGE(TAG, "HTTP read error");
                cfg->state = SPEAKER_STATE_ERROR;
                goto cleanup;
            }
            if (read_len == 0) {
                // End of HTTP response — all PCM data received
                break;
            }

            // Write received PCM bytes into the stream buffer.
            // Blocks if the buffer is full (back-pressures the HTTP read).
            size_t sent = xStreamBufferSend(s_audio_stream, read_buf, (size_t)read_len,
                                            pdMS_TO_TICKS(5000));
            if (sent < (size_t)read_len) {
                ESP_LOGW(TAG, "Stream buffer send partial: %d of %d bytes",
                         (int)sent, read_len);
            }
            total_bytes += sent;

            // Pre-buffer check: once enough data is buffered, signal the
            // playback task to start writing to I2S.
            if (!prebuffer_done &&
                xStreamBufferBytesAvailable(s_audio_stream) >= cfg->prebuffer_threshold) {
                cfg->state = SPEAKER_STATE_PLAYING;
                prebuffer_done = true;
                ESP_LOGI(TAG, "Pre-buffer threshold reached (%d bytes) — starting playback",
                         (int)cfg->prebuffer_threshold);
            }
        }

        ESP_LOGI(TAG, "HTTP transfer complete — %lu total bytes received",
                 (unsigned long)total_bytes);

        // Short audio clip that never filled the pre-buffer: start playback
        // with whatever data we have.
        if (!prebuffer_done && total_bytes > 0) {
            cfg->state = SPEAKER_STATE_PLAYING;
            ESP_LOGI(TAG, "Short clip — starting playback with %lu bytes",
                     (unsigned long)total_bytes);
        }
    }

    // -----------------------------------------------------------------------
    // Step 5: Signal end-of-stream
    //
    // All PCM data from the backend has been written to the stream buffer.
    // Transition to DRAINING: the playback task keeps reading remaining
    // data from the buffer and writing to I2S. When the buffer empties,
    // the playback task transitions to STOPPED → IDLE.
    // -----------------------------------------------------------------------
    if (cfg->state == SPEAKER_STATE_PLAYING) {
        cfg->state = SPEAKER_STATE_DRAINING;
        ESP_LOGI(TAG, "End of stream — draining remaining buffer");
    }

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

done:
    s_network_task_handle = NULL;
    ESP_LOGI(TAG, "Network task finished");
    vTaskDelete(NULL);
}

// ============================================================================
//  PUBLIC API
// ============================================================================

speaker_state_t speaker_get_state(void)
{
    return s_speaker_cfg.state;
}

// --- VOLUME SCALING API ---
void speaker_set_volume(float volume)
{
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    s_speaker_cfg.volume = volume;
    ESP_LOGI(TAG, "Volume set to %.2f", volume);
}
// --- END VOLUME SCALING API ---

void speaker_stop(void)
{
    speaker_config_t *cfg = &s_speaker_cfg;

    // Signal the network task to stop (it checks state each iteration)
    cfg->state = SPEAKER_STATE_IDLE;

    // Kill the network task if it's still running.
    // The HTTP client handle inside the task may leak — acceptable for an
    // immediate-stop operation. A graceful stop would wait for the task to
    // exit on its own after seeing IDLE state.
    if (s_network_task_handle != NULL) {
        vTaskDelete(s_network_task_handle);
        s_network_task_handle = NULL;
    }

    // Brief delay for the playback task to see the IDLE state and exit its loop
    vTaskDelay(pdMS_TO_TICKS(100));

    // Flush any remaining data in the stream buffer.
    // Safe now: network task is dead, playback task has exited its inner loop.
    if (s_audio_stream != NULL) {
        xStreamBufferReset(s_audio_stream);
    }

    // I2S SHUTDOWN: the playback task disables the I2S channel when it
    // exits its inner loop, so we don't need to touch it here.

    ESP_LOGI(TAG, "Playback stopped — state → IDLE");
}

void speaker_play_url(const char *url)
{
    speaker_config_t *cfg = &s_speaker_cfg;

    // If already playing, stop current playback first
    if (cfg->state != SPEAKER_STATE_IDLE && cfg->state != SPEAKER_STATE_STOPPED) {
        ESP_LOGW(TAG, "Already playing — stopping current playback first");
        speaker_stop();
    }

    // Copy URL to static buffer (the network task reads from this; the
    // caller's string may be on the stack and freed before the task runs)
    strncpy(s_play_url, url, MAX_URL_LEN - 1);
    s_play_url[MAX_URL_LEN - 1] = '\0';

    // Reset the stream buffer for new audio data
    if (s_audio_stream != NULL) {
        xStreamBufferReset(s_audio_stream);
    }

    // Enter BUFFERING state — the playback task waits for PLAYING
    cfg->state = SPEAKER_STATE_BUFFERING;

    // Spawn the network producer task.
    // This task opens the HTTP connection, reads PCM chunks, and writes
    // them into the stream buffer. It self-deletes when the transfer
    // completes or on error.
    BaseType_t ret = xTaskCreate(
        speaker_network_task,
        "spk_net",
        NET_TASK_STACK_SIZE,
        NULL,
        SPEAKER_NET_TASK_PRIORITY,
        &s_network_task_handle
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create network task");
        cfg->state = SPEAKER_STATE_ERROR;
    }
}

// ============================================================================
//  PLAYBACK TASK (Consumer)
//
//  Persistent FreeRTOS task created once in main.c. Runs for the lifetime
//  of the application, waking up whenever audio data is available.
//
//  This task has HIGHER PRIORITY than the network task because:
//    - I2S underrun → audible click/pop (bad)
//    - Network read delayed 10ms → no perceptible effect (fine)
//
//  Producer–consumer flow:
//    Network task writes PCM chunks → stream buffer → this task reads
//    → applies volume scaling → writes to I2S DMA → amplifier → speaker
//
//  State-driven behavior:
//    IDLE/BUFFERING/STOPPED: task sleeps (polls state every 20ms)
//    PLAYING:                reads buffer, volume scales, writes I2S
//    DRAINING:               same as PLAYING, but buffer empty → STOPPED
//    ERROR:                  task sleeps (waits for reset)
// ============================================================================
void speaker_playback_task(void *param)
{
    (void)param;
    speaker_config_t *cfg = &s_speaker_cfg;
    int16_t play_buf[PLAYBACK_BUF_SIZE / sizeof(int16_t)];

    ESP_LOGI(TAG, "Playback task started");

    while (1) {
        // -------------------------------------------------------------------
        // Phase 1: Wait for the PLAYING state
        //
        // The network task sets state to PLAYING once the pre-buffer
        // threshold is reached. Until then, we sleep.
        // -------------------------------------------------------------------
        while (cfg->state != SPEAKER_STATE_PLAYING &&
               cfg->state != SPEAKER_STATE_DRAINING) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        // -------------------------------------------------------------------
        // Phase 2: Enable I2S channel
        //
        // I2S clocks (BCLK, WS) start toggling. The amplifier sees clock
        // edges and begins converting whatever data appears on DOUT.
        // -------------------------------------------------------------------
#if !SPEAKER_SIMULATE
        if (!s_i2s_enabled && s_tx_handle != NULL) {
            ESP_ERROR_CHECK(i2s_channel_enable(s_tx_handle));
            s_i2s_enabled = true;
        }
#endif
        ESP_LOGI(TAG, "Playback active — reading from buffer, writing to I2S");

        // -------------------------------------------------------------------
        // Phase 3: Playback loop — stream buffer → volume → I2S
        //
        // Runs until state is no longer PLAYING or DRAINING (either the
        // buffer drains after end-of-stream, or speaker_stop() is called).
        // -------------------------------------------------------------------
        while (cfg->state == SPEAKER_STATE_PLAYING ||
               cfg->state == SPEAKER_STATE_DRAINING) {

            // Read PCM samples from the stream buffer.
            // Blocks up to 50ms if the buffer is empty.
            size_t received = xStreamBufferReceive(
                s_audio_stream, play_buf, sizeof(play_buf),
                pdMS_TO_TICKS(50)
            );

            if (received > 0) {
                // --- VOLUME SCALING ---
                // Multiply each 16-bit PCM sample by the volume factor.
                // This is the software volume control — universal, works
                // with any I2S amplifier, ~1% CPU at 16 kHz.
                // To disable: set VOLUME_SCALING_ENABLED to 0 at top of file.
#if VOLUME_SCALING_ENABLED
                size_t sample_count = received / sizeof(int16_t);
                apply_volume(play_buf, sample_count, cfg->volume);
#endif
                // --- END VOLUME SCALING ---

                // Write PCM samples to I2S peripheral.
                // Blocks until the I2S DMA buffer accepts the data.
#if !SPEAKER_SIMULATE
                size_t bytes_written = 0;
                esp_err_t err = i2s_channel_write(
                    s_tx_handle, play_buf, received,
                    &bytes_written, 1000
                );
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "I2S write error: %s", esp_err_to_name(err));
                    cfg->state = SPEAKER_STATE_ERROR;
                    break;
                }
#else
                ESP_LOGD(TAG, "I2S write: %d bytes (simulated)", (int)received);
                vTaskDelay(pdMS_TO_TICKS(10));
#endif
            } else {
                // Stream buffer empty — two possible situations:
                if (cfg->state == SPEAKER_STATE_DRAINING) {
                    // Network is done and buffer is empty → playback complete
                    cfg->state = SPEAKER_STATE_STOPPED;
                    ESP_LOGI(TAG, "Buffer drained — playback complete");
                    break;
                }

                // Still PLAYING but buffer is empty → underrun.
                // Write silence (zeros) to I2S to prevent clicks/pops.
                memset(play_buf, 0, sizeof(play_buf));
#if !SPEAKER_SIMULATE
                size_t bytes_written = 0;
                i2s_channel_write(
                    s_tx_handle, play_buf, sizeof(play_buf),
                    &bytes_written, 100
                );
#endif
                ESP_LOGW(TAG, "Buffer underrun — writing silence");
            }
        }

        // -------------------------------------------------------------------
        // Phase 4: Disable I2S channel
        //
        // I2S SHUTDOWN: stop the clocks and DMA. The amplifier goes quiet.
        // The channel returns to READY state, ready to be enabled again
        // for the next playback session.
        // -------------------------------------------------------------------
#if !SPEAKER_SIMULATE
        if (s_i2s_enabled && s_tx_handle != NULL) {
            i2s_channel_disable(s_tx_handle);
            s_i2s_enabled = false;
        }
#endif

        // Auto-return to IDLE after normal playback completion
        if (cfg->state == SPEAKER_STATE_STOPPED) {
            cfg->state = SPEAKER_STATE_IDLE;
            ESP_LOGI(TAG, "Playback finished — state → IDLE");
        }
    }
}
