#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
//  MIC DRIVER — Header
//
//  This module captures audio from a digital MEMS microphone (e.g. INMP441)
//  via the I2S peripheral and uploads the raw PCM data to a backend server
//  over HTTP POST. The backend processes the audio (STT → LLM → TTS) and
//  the speaker driver plays the response.
//
//  The driver uses two FreeRTOS tasks connected by a stream buffer:
//    - Capture task (producer): reads I2S RX, applies gain, fills buffer
//    - Upload task  (consumer): reads buffer, sends PCM via HTTP POST
//
//  No audio processing is done on the ESP32 — raw PCM samples go straight
//  from I2S to the network. The backend handles speech-to-text, noise
//  reduction, and any other processing.
//
//  Lifecycle:
//    mic_init() → mic_start() → [recording] → mic_stop() → mic_upload()
//
// ============================================================================
//  WHERE THIS FITS IN THE SYSTEM
// ============================================================================
//
//  Audio data flows through the following pipeline:
//
//    Microphone (INMP441 / digital MEMS, I2S output)
//      │ I2S bus: BCLK, WS, DOUT (mic's data out → ESP32 DIN)
//      ▼
//    ESP32-S3 Capture Task (producer)
//      │ reads I2S RX, applies optional software gain
//      ▼
//    Stream Buffer (16 KB ring buffer)
//      │ decouples I2S capture timing from network uploads
//      ▼
//    ESP32-S3 Upload Task (consumer)
//      │ reads from buffer, sends as HTTP POST body
//      ▼
//    Cloud Backend (STT + LLM + TTS)
//      │ processes speech, generates audio response
//      ▼
//    Speaker Driver (separate pipeline)
//      │ plays backend's response via I2S TX
//      ▼
//    Speaker
//
//  This driver mirrors the speaker driver but in reverse direction:
//    Speaker: network → buffer → I2S TX (output)
//    Mic:     I2S RX (input) → buffer → network
//
// ============================================================================
//  HOW OTHER FILES INTERFACE WITH THIS DRIVER
// ============================================================================
//
//  The voice assistant (voice_assistant.c) orchestrates the mic driver
//  using push-to-talk button events:
//
//    // User presses button — start recording
//    mic_start();
//
//    // User releases button — stop recording
//    mic_stop();
//
//    // Upload captured audio to backend
//    mic_upload("http://your-backend.com/api/listen");
//
//    // Wait for upload to complete
//    while (mic_get_state() != MIC_STATE_IDLE &&
//           mic_get_state() != MIC_STATE_ERROR) {
//        vTaskDelay(pdMS_TO_TICKS(100));
//    }
//
//    // Then play backend response via speaker driver
//    speaker_play_url("http://your-backend.com/api/speak");
//
//    // Adjust input gain at any time
//    mic_set_gain(2.0f);
//
// ============================================================================

typedef enum {
    MIC_STATE_IDLE,         // initialized, not recording
    MIC_STATE_RECORDING,    // I2S RX active, capturing to buffer
    MIC_STATE_UPLOADING,    // capture done, sending buffer to backend
    MIC_STATE_DONE,         // upload complete (transitions to IDLE)
    MIC_STATE_ERROR,
} mic_state_t;

typedef struct {
    int bclk_pin;               // I2S bit clock GPIO (own or shared with speaker)
    int ws_pin;                 // I2S word select (LRCLK) GPIO
    int din_pin;                // I2S data in GPIO (mic's data out)
    int button_pin;             // Push-to-talk GPIO (-1 = not used by mic driver;
                                //   button handling lives in voice_assistant.c)
    uint32_t sample_rate;       // e.g. 16000
    uint8_t bits_per_sample;    // 16
    uint8_t channels;           // 1 = mono
    size_t ring_buffer_size;    // in bytes (e.g. 16384)
    float gain;                 // 1.0 = unity, >1.0 = amplify
    mic_state_t state;
} mic_config_t;

// ---------------------------------------------------------------------------
//  mic_init
//
//  Called once at startup. Configures the I2S peripheral in standard
//  Philips mode (RX only or full-duplex with speaker) and allocates the
//  stream buffer used to decouple I2S reads from network uploads.
//
//  Pin assignments and audio format are compiled into mic_driver.c.
//  After this call, the I2S RX channel exists but is disabled — it will
//  be enabled when recording starts.
// ---------------------------------------------------------------------------
void mic_init(void);

// ---------------------------------------------------------------------------
//  mic_start
//
//  Begins recording. The capture task (mic_capture_task) starts reading
//  I2S RX samples, applying gain, and writing PCM into the stream buffer.
//  Resets the stream buffer and byte counter for a fresh recording.
//
//  Call this when the push-to-talk button is pressed.
// ---------------------------------------------------------------------------
void mic_start(void);

// ---------------------------------------------------------------------------
//  mic_stop
//
//  Stops recording. The capture task exits its inner loop and disables
//  the I2S RX channel. Captured PCM data remains in the stream buffer
//  for subsequent upload via mic_upload().
//
//  Call this when the push-to-talk button is released.
// ---------------------------------------------------------------------------
void mic_stop(void);

// ---------------------------------------------------------------------------
//  mic_upload
//
//  Uploads the captured PCM audio to the given URL as an HTTP POST body.
//  Spawns an internal upload task that reads from the stream buffer and
//  sends the data. Returns immediately (non-blocking).
//
//  The upload task sets the following headers:
//    Content-Type: application/octet-stream
//    X-Audio-Format: pcm;rate=16000;bits=16;channels=1
//    Content-Length: <total captured bytes>
//
//  State transitions: IDLE → UPLOADING → IDLE
//  On error: state → ERROR
// ---------------------------------------------------------------------------
void mic_upload(const char *url);

// ---------------------------------------------------------------------------
//  mic_set_gain
//
//  Sets the software input gain factor. Range: 0.0 (mute) to any positive
//  value (>1.0 amplifies). Takes effect on the next I2S read by the
//  capture task. Has no effect if GAIN_SCALING_ENABLED is 0 in mic_driver.c.
//
//  This is a simple fixed multiplier. For more advanced gain control
//  (e.g. AGC — automatic gain control that tracks signal level), replace
//  the apply_gain() function in mic_driver.c with a stateful algorithm.
// ---------------------------------------------------------------------------
void mic_set_gain(float gain);

// ---------------------------------------------------------------------------
//  mic_get_state
//
//  Returns the current mic driver state. Use MIC_STATE_IDLE to check
//  if recording/upload has finished.
// ---------------------------------------------------------------------------
mic_state_t mic_get_state(void);

// ---------------------------------------------------------------------------
//  mic_capture_task
//
//  FreeRTOS task entry point for the I2S capture producer. Created once
//  in main.c and runs for the lifetime of the application. Should be
//  given HIGHER priority than network tasks (TASK_PRIORITY + 1) to
//  prevent I2S RX overflows — an overflow produces gaps in the recording,
//  while a network upload can tolerate 10ms+ delays without consequence.
//
//  Flow:
//    1. Wait for state == RECORDING (set by mic_start())
//    2. Enable I2S RX channel
//    3. Read PCM samples from I2S peripheral
//    4. Apply software gain scaling (if enabled)
//    5. Write to stream buffer (blocks if full — oldest data preserved)
//    6. Loop until state != RECORDING (set by mic_stop())
//    7. Disable I2S RX channel, return to step 1
// ---------------------------------------------------------------------------
void mic_capture_task(void *param);

#ifdef __cplusplus
}
#endif
