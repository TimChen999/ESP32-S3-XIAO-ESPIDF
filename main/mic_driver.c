#include "mic_driver.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_http_client.h"

#ifndef MIC_SIMULATE
#define MIC_SIMULATE 0
#endif

#if !MIC_SIMULATE
#include "driver/i2s_std.h"
#endif

static const char *TAG = "MIC_DRV";

// ============================================================================
//  AUDIO FORMAT CONFIGURATION
//
//  Select a preset by changing AUDIO_PRESET. These are compile-time
//  constants — the audio format does not change at runtime.
//
//  AUDIO_PRESET_SPEECH: 16 kHz / 16-bit / mono   (32 KB/s, for STT APIs)
//  AUDIO_PRESET_HIFI:   48 kHz / 16-bit / mono   (96 KB/s, high fidelity)
//
//  The preset MUST match the backend's expected STT input format.
//  Most STT APIs (OpenAI Whisper, Google Speech-to-Text, Deepgram)
//  accept 16 kHz mono natively. Higher sample rates waste bandwidth
//  without improving recognition accuracy.
//
//  Bandwidth at each preset:
//    Speech: 16000 × 2 × 1 = 32,000 B/s   = 256 kbps
//    HiFi:   48000 × 2 × 1 = 96,000 B/s   = 768 kbps
// ============================================================================
#define AUDIO_PRESET_SPEECH     0
#define AUDIO_PRESET_HIFI       1
#define AUDIO_PRESET            AUDIO_PRESET_SPEECH

#if AUDIO_PRESET == AUDIO_PRESET_SPEECH
#define MIC_SAMPLE_RATE         16000
#define MIC_BITS                16
#define MIC_CHANNELS            1
#elif AUDIO_PRESET == AUDIO_PRESET_HIFI
#define MIC_SAMPLE_RATE         48000
#define MIC_BITS                16
#define MIC_CHANNELS            1
#endif

// ============================================================================
//  I2S PERIPHERAL SELECTION — Separate vs. Full-Duplex
//
//  The ESP32-S3 has two I2S peripherals (I2S0 and I2S1). The mic can use
//  either one:
//
//  MIC_I2S_NUM = 1 (default) — SEPARATE I2S1
//    - Mic gets its own I2S peripheral, completely independent of speaker
//    - Uses 3 GPIO pins: BCLK, WS, DIN (own clock lines)
//    - Zero coordination with speaker driver — simplest setup
//    - No changes to speaker_driver.c needed
//
//  MIC_I2S_NUM = 0 — FULL-DUPLEX on I2S0 (shared with speaker)
//    - Mic shares BCLK + WS clock lines with the speaker
//    - Uses only 1 new GPIO pin: DIN (mic data in)
//    - Saves 2 GPIO pins but requires speaker_driver.c coordination:
//        1. Set SPEAKER_FULL_DUPLEX=1 in speaker_driver.c
//        2. speaker_init() must be called BEFORE mic_init()
//        3. speaker_init() creates both TX+RX handles
//        4. mic_init() retrieves RX handle via speaker_get_rx_handle()
//    - After init, both drivers operate completely independently
//
//  Pin usage comparison:
//    Separate (I2S1):      3 pins — BCLK(GPIO1), WS(GPIO2), DIN(GPIO3)
//    Full-duplex (I2S0):   1 pin  — DIN(GPIO1) (BCLK+WS shared with speaker)
// ============================================================================
#define MIC_I2S_NUM             1

// ============================================================================
//  I2S PIN CONFIGURATION
//
//  Pin assignments depend on MIC_I2S_NUM (separate vs. full-duplex):
//
//  Separate I2S1 (MIC_I2S_NUM = 1):
//    BCLK → GPIO1 (D0 on XIAO) — mic's own bit clock
//    WS   → GPIO2 (D1 on XIAO) — mic's own word select
//    DIN  → GPIO3 (D2 on XIAO) — mic data input (INMP441 SD pin)
//
//  Full-duplex I2S0 (MIC_I2S_NUM = 0):
//    BCLK → GPIO9  (D10 on XIAO) — shared with speaker, must match
//    WS   → GPIO10 (D7 on XIAO)  — shared with speaker, must match
//    DIN  → GPIO1  (D0 on XIAO)  — only new pin needed
//
//  These pins are free when using WiFi mode (modem UART pins D0-D5 unused).
//  For the INMP441 mic, wire: BCLK→SCK, WS→WS, DIN→SD, VDD→3.3V, GND→GND.
// ============================================================================
#if MIC_I2S_NUM == 0
#define MIC_I2S_BCLK_PIN       9
#define MIC_I2S_WS_PIN         10
#define MIC_I2S_DIN_PIN        1
#else
#define MIC_I2S_BCLK_PIN       1
#define MIC_I2S_WS_PIN         2
#define MIC_I2S_DIN_PIN        3
#endif

// ============================================================================
//  RING BUFFER CONFIGURATION
//
//  The stream buffer sits between the capture task (producer) and the
//  upload task (consumer). Since recording completes before upload begins
//  (push-to-talk model), the buffer must hold the entire recording.
//
//  At 16 kHz / 16-bit / mono (32 KB/s):
//    16 KB = 500 ms of audio
//
//  IMPORTANT — Max recording duration = RING_BUFFER_SIZE / bytes_per_sec:
//    16 KB  =  0.5 s   ( 3% of 512 KB SRAM)
//    32 KB  =  1.0 s   ( 6% of SRAM)
//    64 KB  =  2.0 s   (12% of SRAM)
//    128 KB =  4.0 s   (25% of SRAM)
//    160 KB =  5.0 s   (31% of SRAM)
//
//  Increase this for longer push-to-talk recordings. When the buffer is
//  full during recording, new samples are discarded (logged as warning).
// ============================================================================
#define RING_BUFFER_SIZE        16384
#define STREAM_TRIGGER_LEVEL    1

// ============================================================================
//  GAIN CONFIGURATION — Software Input Scaling
//
//  Multiplies each 16-bit PCM sample by a gain factor before storing
//  in the buffer. Useful when the MEMS mic output is too quiet for STT.
//
//  Set GAIN_SCALING_ENABLED to 0 to bypass all gain processing (samples
//  pass through from I2S to buffer unmodified).
//
//  This is a simple fixed multiplier applied per-sample. To implement
//  more advanced gain control (e.g. AGC — automatic gain control that
//  dynamically adjusts based on signal level), replace the apply_gain()
//  function below with a stateful algorithm that tracks RMS or peak
//  levels across a sliding window.
// ============================================================================
#define GAIN_SCALING_ENABLED    1
#define DEFAULT_GAIN            1.0f

// ============================================================================
//  TASK & BUFFER SIZES
// ============================================================================
#define CAPTURE_BUF_SIZE        1024
#define UPLOAD_READ_BUF_SIZE    1024
#define UPLOAD_TASK_STACK_SIZE  4096
#define MIC_UPLOAD_TASK_PRIORITY  5
#define MAX_URL_LEN             512

// ============================================================================
//  STATIC STATE
// ============================================================================
static mic_config_t s_mic_cfg = {
    .bclk_pin          = MIC_I2S_BCLK_PIN,
    .ws_pin            = MIC_I2S_WS_PIN,
    .din_pin           = MIC_I2S_DIN_PIN,
    .button_pin        = -1,
    .sample_rate       = MIC_SAMPLE_RATE,
    .bits_per_sample   = MIC_BITS,
    .channels          = MIC_CHANNELS,
    .ring_buffer_size  = RING_BUFFER_SIZE,
    .gain              = DEFAULT_GAIN,
    .state             = MIC_STATE_IDLE,
};

#if !MIC_SIMULATE
static i2s_chan_handle_t s_rx_handle = NULL;
#endif
static StreamBufferHandle_t s_audio_stream = NULL;
static TaskHandle_t s_upload_task_handle = NULL;
static bool s_i2s_enabled = false;
static char s_upload_url[MAX_URL_LEN];
static size_t s_captured_bytes = 0;

// ============================================================================
//  FULL-DUPLEX SUPPORT
//
//  When MIC_I2S_NUM == 0, the mic shares I2S0 with the speaker in full-
//  duplex mode. The speaker driver must be compiled with SPEAKER_FULL_DUPLEX=1,
//  and speaker_init() must run BEFORE mic_init(). The speaker creates both
//  TX and RX handles; mic_init() retrieves the RX handle via this function.
// ============================================================================
#if MIC_I2S_NUM == 0 && !MIC_SIMULATE
extern i2s_chan_handle_t speaker_get_rx_handle(void);
#endif

// ============================================================================
//  GAIN SCALING
//
//  Multiplies each 16-bit PCM sample by the gain factor. Called by the
//  capture task on every buffer of I2S data before writing to the stream
//  buffer.
//
//  Clipping: values are clamped to INT16_MIN..INT16_MAX to prevent
//  wraparound distortion when gain > 1.0. If clipping occurs frequently,
//  reduce the gain or move the mic further from the sound source.
//
//  To replace with AGC: keep the same function signature, but add static
//  state variables to track signal level across calls. Adjust gain
//  dynamically to maintain a target RMS level (e.g. -20 dBFS).
// ============================================================================
#if GAIN_SCALING_ENABLED
static void apply_gain(int16_t *samples, size_t count, float gain)
{
    for (size_t i = 0; i < count; i++) {
        int32_t amplified = (int32_t)(samples[i] * gain);
        if (amplified > INT16_MAX) amplified = INT16_MAX;
        if (amplified < INT16_MIN) amplified = INT16_MIN;
        samples[i] = (int16_t)amplified;
    }
}
#endif

// ============================================================================
//  I2S RX INITIALIZATION
//
//  Configures the I2S peripheral for standard Philips mode (RX).
//  This supports standard I2S MEMS microphones:
//    - INMP441:  wire SCK→BCLK, WS→WS, SD→DIN, VDD→3.3V, GND→GND, L/R→GND
//    - SPH0645:  same pinout as INMP441
//    - ICS-43434: same pinout, higher SNR
//
//  PDM microphones (e.g. MSM261S4030H0 built into XIAO Sense) require
//  a different init: i2s_channel_init_pdm_rx_mode(). Not supported here.
//
//  Initialization sequence:
//    1. Acquire I2S RX channel handle (separate or from speaker full-duplex)
//    2. i2s_channel_init_std_mode() → configure clock, slots, GPIO for RX
//    3. Channel stays disabled until mic_start() triggers recording
// ============================================================================
void mic_init(void)
{
    mic_config_t *cfg = &s_mic_cfg;

#if !MIC_SIMULATE
    // -----------------------------------------------------------------------
    //  Step 1: Acquire I2S RX channel handle
    //
    //  MIC_I2S_NUM == 0 (full-duplex):
    //    Speaker driver already created both TX+RX handles on I2S0.
    //    Retrieve the RX handle. Requires SPEAKER_FULL_DUPLEX=1 in
    //    speaker_driver.c and speaker_init() called before mic_init().
    //
    //  MIC_I2S_NUM == 1 (separate):
    //    Create a new RX-only channel on I2S1. Completely independent
    //    of the speaker — no coordination needed.
    // -----------------------------------------------------------------------
#if MIC_I2S_NUM == 0
    s_rx_handle = speaker_get_rx_handle();
    if (s_rx_handle == NULL) {
        ESP_LOGE(TAG, "Full-duplex: speaker RX handle is NULL");
        ESP_LOGE(TAG, "  Ensure SPEAKER_FULL_DUPLEX=1 in speaker_driver.c");
        ESP_LOGE(TAG, "  Ensure speaker_init() is called before mic_init()");
        cfg->state = MIC_STATE_ERROR;
        return;
    }
    ESP_LOGI(TAG, "Full-duplex: using shared I2S0 RX handle from speaker");
#else
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_handle));
    ESP_LOGI(TAG, "Separate: created independent I2S1 RX channel");
#endif

    // -----------------------------------------------------------------------
    //  Step 2: Configure I2S standard (Philips) mode for RX
    //
    //  Clock:  sample rate determines BCLK and WS timing
    //  Slots:  16-bit samples, mono layout (left channel from INMP441)
    //  GPIO:   BCLK, WS, DIN pin assignments (DOUT unused — no speaker
    //          on this channel; or I2S_GPIO_UNUSED if full-duplex)
    // -----------------------------------------------------------------------
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
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)cfg->din_pin,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_handle, &std_cfg));

#else
    ESP_LOGI(TAG, "MIC_SIMULATE=1 — I2S hardware disabled (generating fake data instead)");
#endif

    // Step 3: Allocate stream buffer (ring buffer between capture and upload tasks)
    s_audio_stream = xStreamBufferCreate(cfg->ring_buffer_size, STREAM_TRIGGER_LEVEL);
    if (s_audio_stream == NULL) {
        ESP_LOGE(TAG, "Failed to create stream buffer (%d bytes)", (int)cfg->ring_buffer_size);
        cfg->state = MIC_STATE_ERROR;
        return;
    }

    cfg->state = MIC_STATE_IDLE;
    ESP_LOGI(TAG, "mic_init: rate=%lu bits=%d ch=%d buf=%d pins=BCLK:%d/WS:%d/DIN:%d i2s=%d",
             (unsigned long)cfg->sample_rate, cfg->bits_per_sample, cfg->channels,
             (int)cfg->ring_buffer_size, cfg->bclk_pin, cfg->ws_pin, cfg->din_pin,
             MIC_I2S_NUM);
}

// ============================================================================
//  UPLOAD TASK (Consumer — Ephemeral)
//
//  Internal task spawned by mic_upload(). Reads captured PCM from the
//  stream buffer and sends it to the backend as an HTTP POST body.
//
//  Upload flow:
//
//    Step 1: Open HTTP POST to backend /api/listen
//            Headers tell the backend the PCM format so it can feed
//            the correct parameters to its STT engine.
//
//    Step 2: Write captured PCM bytes from stream buffer into HTTP body
//            Reads in chunks until the buffer is drained.
//
//    Step 3: Read response status (200 OK expected)
//            The response body is not used — the speaker driver will
//            separately request the audio response from the backend.
//
//    Step 4: Cleanup, set state to IDLE, self-delete
//
//  Unlike the speaker driver which streams in real-time (chunked HTTP
//  response), this upload sends a complete recording as the request body.
//  The recording is fully captured before upload begins (push-to-talk
//  defines the start and end of the recording).
// ============================================================================
static void mic_upload_task(void *param)
{
    (void)param;
    mic_config_t *cfg = &s_mic_cfg;

    ESP_LOGI(TAG, "Upload task started — URL: %s, bytes: %d",
             s_upload_url, (int)s_captured_bytes);

    // -----------------------------------------------------------------------
    // Step 1: Configure and open HTTP connection
    // -----------------------------------------------------------------------
    esp_http_client_config_t http_cfg = {
        .url        = s_upload_url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = 30000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        cfg->state = MIC_STATE_ERROR;
        goto done;
    }

    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");

    char fmt_header[64];
    snprintf(fmt_header, sizeof(fmt_header), "pcm;rate=%lu;bits=%d;channels=%d",
             (unsigned long)cfg->sample_rate, cfg->bits_per_sample, cfg->channels);
    esp_http_client_set_header(client, "X-Audio-Format", fmt_header);

    esp_err_t err = esp_http_client_open(client, (int)s_captured_bytes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        cfg->state = MIC_STATE_ERROR;
        goto cleanup;
    }

    // -----------------------------------------------------------------------
    // Step 2: Write captured PCM from stream buffer into HTTP body
    //
    // The stream buffer holds all PCM data from the recording session.
    // Read chunks and write them to the HTTP client until the buffer
    // is empty (all captured bytes sent).
    // -----------------------------------------------------------------------
    {
        char write_buf[UPLOAD_READ_BUF_SIZE];
        size_t total_sent = 0;

        while (total_sent < s_captured_bytes) {
            if (cfg->state == MIC_STATE_IDLE) {
                ESP_LOGI(TAG, "Upload cancelled");
                goto cleanup;
            }

            size_t received = xStreamBufferReceive(
                s_audio_stream, write_buf, sizeof(write_buf),
                pdMS_TO_TICKS(1000)
            );

            if (received == 0) {
                ESP_LOGW(TAG, "Stream buffer empty before all bytes sent (%d of %d)",
                         (int)total_sent, (int)s_captured_bytes);
                break;
            }

            int written = esp_http_client_write(client, write_buf, (int)received);
            if (written < 0) {
                ESP_LOGE(TAG, "HTTP write error");
                cfg->state = MIC_STATE_ERROR;
                goto cleanup;
            }
            total_sent += (size_t)written;
        }

        ESP_LOGI(TAG, "Upload complete — %lu bytes sent",
                 (unsigned long)total_sent);
    }

    // -----------------------------------------------------------------------
    // Step 3: Read response
    // -----------------------------------------------------------------------
    esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    if (status_code != 200) {
        ESP_LOGE(TAG, "Backend returned HTTP %d (expected 200)", status_code);
        cfg->state = MIC_STATE_ERROR;
        goto cleanup;
    }
    ESP_LOGI(TAG, "Backend acknowledged — HTTP %d", status_code);

    cfg->state = MIC_STATE_DONE;
    ESP_LOGI(TAG, "Upload finished — state → DONE");

    vTaskDelay(pdMS_TO_TICKS(50));
    cfg->state = MIC_STATE_IDLE;
    ESP_LOGI(TAG, "State → IDLE");

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

done:
    s_upload_task_handle = NULL;
    ESP_LOGI(TAG, "Upload task finished");
    vTaskDelete(NULL);
}

// ============================================================================
//  PUBLIC API
// ============================================================================

mic_state_t mic_get_state(void)
{
    return s_mic_cfg.state;
}

void mic_set_gain(float gain)
{
    if (gain < 0.0f) gain = 0.0f;
    s_mic_cfg.gain = gain;
    ESP_LOGI(TAG, "Gain set to %.2f", gain);
}

void mic_start(void)
{
    mic_config_t *cfg = &s_mic_cfg;

    if (cfg->state == MIC_STATE_RECORDING) {
        ESP_LOGW(TAG, "Already recording");
        return;
    }

    if (cfg->state == MIC_STATE_UPLOADING) {
        ESP_LOGW(TAG, "Upload in progress — stopping upload first");
        if (s_upload_task_handle != NULL) {
            vTaskDelete(s_upload_task_handle);
            s_upload_task_handle = NULL;
        }
    }

    cfg->state = MIC_STATE_IDLE;
    vTaskDelay(pdMS_TO_TICKS(50));

    s_captured_bytes = 0;
    if (s_audio_stream != NULL) {
        xStreamBufferReset(s_audio_stream);
    }

    cfg->state = MIC_STATE_RECORDING;
    ESP_LOGI(TAG, "Recording started");
}

void mic_stop(void)
{
    mic_config_t *cfg = &s_mic_cfg;

    if (cfg->state != MIC_STATE_RECORDING) {
        return;
    }

    cfg->state = MIC_STATE_IDLE;
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "Recording stopped — %lu bytes captured",
             (unsigned long)s_captured_bytes);
}

void mic_upload(const char *url)
{
    mic_config_t *cfg = &s_mic_cfg;

    if (s_captured_bytes == 0) {
        ESP_LOGW(TAG, "No audio captured — nothing to upload");
        return;
    }

    strncpy(s_upload_url, url, MAX_URL_LEN - 1);
    s_upload_url[MAX_URL_LEN - 1] = '\0';

    cfg->state = MIC_STATE_UPLOADING;

    BaseType_t ret = xTaskCreate(
        mic_upload_task,
        "mic_upload",
        UPLOAD_TASK_STACK_SIZE,
        NULL,
        MIC_UPLOAD_TASK_PRIORITY,
        &s_upload_task_handle
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create upload task");
        cfg->state = MIC_STATE_ERROR;
    }
}

// ============================================================================
//  CAPTURE TASK (Producer — Permanent)
//
//  Persistent FreeRTOS task created once in main.c. Runs for the lifetime
//  of the application, waking up whenever recording is requested.
//
//  This task has HIGHER PRIORITY than the upload task because:
//    - I2S RX overflow → lost mic samples, gaps in recording (bad)
//    - Network upload delayed 10ms → no perceptible effect (fine)
//
//  Producer–consumer flow:
//    I2S RX peripheral reads mic → this task reads I2S → applies gain →
//    writes to stream buffer → upload task reads buffer → HTTP POST
//
//  State-driven behavior:
//    IDLE/UPLOADING/DONE: task sleeps (polls state every 20ms)
//    RECORDING:           reads I2S, gain scales, writes to buffer
//    ERROR:               task sleeps (waits for reset)
// ============================================================================
void mic_capture_task(void *param)
{
    (void)param;
    mic_config_t *cfg = &s_mic_cfg;
    int16_t capture_buf[CAPTURE_BUF_SIZE / sizeof(int16_t)];

    ESP_LOGI(TAG, "Capture task started");

    while (1) {
        // -------------------------------------------------------------------
        // Phase 1: Wait for the RECORDING state
        //
        // mic_start() sets state to RECORDING. Until then, we sleep.
        // -------------------------------------------------------------------
        while (cfg->state != MIC_STATE_RECORDING) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        // -------------------------------------------------------------------
        // Phase 2: Enable I2S RX channel
        //
        // I2S clocks start toggling. The mic sees clock edges and begins
        // shifting out digitized audio samples on its SD/DOUT pin.
        // -------------------------------------------------------------------
#if !MIC_SIMULATE
        if (!s_i2s_enabled && s_rx_handle != NULL) {
            ESP_ERROR_CHECK(i2s_channel_enable(s_rx_handle));
            s_i2s_enabled = true;
        }
#endif
        ESP_LOGI(TAG, "Capture active — reading I2S, writing to buffer");

        // -------------------------------------------------------------------
        // Phase 3: Capture loop — I2S RX → gain → stream buffer
        //
        // Runs until state is no longer RECORDING (mic_stop() called).
        // -------------------------------------------------------------------
        while (cfg->state == MIC_STATE_RECORDING) {
#if !MIC_SIMULATE
            size_t bytes_read = 0;
            esp_err_t err = i2s_channel_read(
                s_rx_handle, capture_buf, sizeof(capture_buf),
                &bytes_read, 1000
            );
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "I2S read error: %s", esp_err_to_name(err));
                cfg->state = MIC_STATE_ERROR;
                break;
            }
#else
            memset(capture_buf, 0, sizeof(capture_buf));
            size_t bytes_read = sizeof(capture_buf);
            uint32_t sim_delay_ms = (CAPTURE_BUF_SIZE * 1000) /
                                    (MIC_SAMPLE_RATE * (MIC_BITS / 8) * MIC_CHANNELS);
            vTaskDelay(pdMS_TO_TICKS(sim_delay_ms));
            ESP_LOGD(TAG, "Simulated I2S read: %d bytes", (int)bytes_read);
#endif

            if (bytes_read > 0) {
#if GAIN_SCALING_ENABLED
                size_t sample_count = bytes_read / sizeof(int16_t);
                apply_gain(capture_buf, sample_count, cfg->gain);
#endif

                size_t sent = xStreamBufferSend(
                    s_audio_stream, capture_buf, bytes_read,
                    pdMS_TO_TICKS(100)
                );

                if (sent < bytes_read) {
                    ESP_LOGW(TAG, "Buffer full — discarded %d of %d bytes "
                             "(increase RING_BUFFER_SIZE for longer recordings)",
                             (int)(bytes_read - sent), (int)bytes_read);
                }
                s_captured_bytes += sent;
            }
        }

        // -------------------------------------------------------------------
        // Phase 4: Disable I2S RX channel
        //
        // Stop the clocks and DMA. The mic goes idle. The channel returns
        // to READY state, ready to be enabled again for the next recording.
        // -------------------------------------------------------------------
#if !MIC_SIMULATE
        if (s_i2s_enabled && s_rx_handle != NULL) {
            i2s_channel_disable(s_rx_handle);
            s_i2s_enabled = false;
        }
#endif
        ESP_LOGI(TAG, "Capture stopped — %lu bytes in buffer",
                 (unsigned long)s_captured_bytes);
    }
}
