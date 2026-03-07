#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
//  SPEAKER DRIVER — Header
//
//  This module provides audio playback through an I2S amplifier/speaker.
//  It receives raw PCM audio data over HTTP from a backend server and
//  plays it through an I2S-connected amplifier (e.g. MAX98357A, NS4168).
//
//  The driver uses two FreeRTOS tasks connected by a stream buffer:
//    - Network task (producer): fetches PCM data via HTTP, fills buffer
//    - Playback task (consumer): reads buffer, applies volume, writes I2S
//
//  No audio decoding is performed on the ESP32 — the backend handles all
//  format conversion. The ESP32 receives raw PCM bytes and writes them
//  directly to the I2S peripheral.
//
//  Lifecycle:
//    speaker_init() → speaker_play_url() → [playback] → speaker_stop()
//
// ============================================================================
//  WHERE THIS FITS IN THE SYSTEM
// ============================================================================
//
//  Audio data flows through the following pipeline:
//
//    Cloud Backend (LLM + TTS)
//      │ raw PCM bytes over HTTP (chunked transfer encoding)
//      ▼
//    ESP32-S3 Network Task (producer)
//      │ writes PCM to stream buffer
//      ▼
//    Stream Buffer (16 KB ring buffer)
//      │ decouples network jitter from audio playback
//      ▼
//    ESP32-S3 Playback Task (consumer)
//      │ reads from buffer, applies software volume scaling, writes to I2S
//      ▼
//    I2S Amplifier (MAX98357A / NS4168)
//      │ digital-to-analog conversion + class D amplification
//      ▼
//    Speaker (4Ω/8Ω, 2–3W)
//
//  This driver targets I2S-only amplifiers. These need no I2C register
//  configuration — just 3 GPIO wires (BCLK, WS, DOUT) plus power. Codec
//  chips (ES8311, WM8960) that require I2C setup are not supported.
//
// ============================================================================
//  HOW OTHER FILES INTERFACE WITH THIS DRIVER
// ============================================================================
//
//  After speaker_init() and starting the playback task, any module with
//  network access can trigger audio playback:
//
//    // Wait for network to be ready
//    network_app_wait_for_connection();
//
//    // Play audio from backend
//    speaker_play_url("http://your-backend.com/api/speak");
//
//    // Poll for completion
//    while (speaker_get_state() != SPEAKER_STATE_STOPPED &&
//           speaker_get_state() != SPEAKER_STATE_IDLE) {
//        vTaskDelay(pdMS_TO_TICKS(100));
//    }
//
//    // Adjust volume at any time
//    speaker_set_volume(0.5f);
//
//    // Stop immediately
//    speaker_stop();
//
// ============================================================================

typedef enum {
    SPEAKER_STATE_IDLE,       // initialized, not playing
    SPEAKER_STATE_BUFFERING,  // receiving data, filling pre-buffer
    SPEAKER_STATE_PLAYING,    // actively writing to I2S
    SPEAKER_STATE_DRAINING,   // network done, playing remaining buffer
    SPEAKER_STATE_STOPPED,    // playback complete
    SPEAKER_STATE_ERROR,
} speaker_state_t;

typedef struct {
    int bclk_pin;               // I2S bit clock GPIO
    int ws_pin;                 // I2S word select (LRCLK) GPIO
    int dout_pin;               // I2S data out GPIO
    uint32_t sample_rate;       // e.g. 16000
    uint8_t bits_per_sample;    // 16
    uint8_t channels;           // 1 = mono, 2 = stereo
    size_t ring_buffer_size;    // in bytes (e.g. 16384)
    size_t prebuffer_threshold; // bytes to fill before starting I2S
    float volume;               // 0.0 to 1.0
    speaker_state_t state;
} speaker_config_t;

// ---------------------------------------------------------------------------
//  speaker_init
//
//  Called once at startup. Configures the I2S peripheral in standard
//  Philips mode (TX only, no RX) and allocates the stream buffer used
//  to decouple network reads from I2S writes.
//
//  Pin assignments and audio format are compiled into speaker_driver.c.
//  After this call, the I2S channel exists but is disabled — it will be
//  enabled when playback starts.
// ---------------------------------------------------------------------------
void speaker_init(void);

// ---------------------------------------------------------------------------
//  speaker_play_url
//
//  Initiates audio streaming from the given HTTP URL. Spawns an internal
//  network task that connects to the backend and streams the PCM response
//  into the ring buffer.
//
//  Audio pipeline:
//    1. ESP32 sends HTTP POST to backend
//       (TODO: future mic audio goes in request body — see mic TODO in .c)
//    2. Backend processes request (LLM parses text, TTS generates speech)
//    3. Backend transcodes output to raw PCM (s16le, matching our config)
//    4. Backend streams PCM back via chunked transfer encoding
//    5. Network task writes chunks into stream buffer
//    6. Playback task reads buffer, applies volume, writes to I2S
//
//  If called while already playing, stops current playback first.
// ---------------------------------------------------------------------------
void speaker_play_url(const char *url);

// ---------------------------------------------------------------------------
//  speaker_stop
//
//  Stops playback immediately. Kills the network task, flushes the stream
//  buffer, and returns to IDLE. The playback task will disable I2S on
//  its next iteration.
// ---------------------------------------------------------------------------
void speaker_stop(void);

// ---------------------------------------------------------------------------
//  speaker_set_volume
//
//  Sets the software volume scaling factor. Range: 0.0 (mute) to 1.0
//  (full scale). Takes effect on the next buffer read by the playback
//  task. Has no effect if VOLUME_SCALING_ENABLED is 0 in speaker_driver.c.
// ---------------------------------------------------------------------------
void speaker_set_volume(float volume);

// ---------------------------------------------------------------------------
//  speaker_get_state
//
//  Returns the current speaker driver state. Use SPEAKER_STATE_IDLE or
//  SPEAKER_STATE_STOPPED to check if playback has finished.
// ---------------------------------------------------------------------------
speaker_state_t speaker_get_state(void);

// ---------------------------------------------------------------------------
//  speaker_playback_task
//
//  FreeRTOS task entry point for the I2S playback consumer. Created once
//  in main.c and runs for the lifetime of the application. Should be
//  given HIGHER priority than network tasks (TASK_PRIORITY + 1) to
//  prevent I2S underruns — an I2S underrun produces audible clicks/pops,
//  while a network read can tolerate 10ms+ delays without consequence.
//
//  Flow:
//    1. Wait for state == PLAYING (set by network task after pre-buffer)
//    2. Enable I2S channel
//    3. Read PCM data from stream buffer
//    4. Apply software volume scaling (if enabled)
//    5. Write to I2S channel (blocks until DMA accepts data)
//    6. On underrun (buffer empty, still PLAYING), write silence
//    7. On drain complete (buffer empty, DRAINING), stop
//    8. Disable I2S channel, return to step 1
// ---------------------------------------------------------------------------
void speaker_playback_task(void *param);

#ifdef __cplusplus
}
#endif
