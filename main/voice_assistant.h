#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
//  VOICE ASSISTANT — Header
//
//  Application-layer task that orchestrates the audio pipeline. This is the
//  glue between triggers (button, wake word, timer) and the speaker driver.
//
//  This module does NOT own any hardware. It sits above the speaker driver
//  and calls its public API:
//
//    ┌─────────────────────────────┐
//    │  voice_assistant_task       │  ← APPLICATION LAYER (this file)
//    │  decides WHEN to play       │
//    │                             │
//    │  calls: speaker_play_url()  │
//    │  polls: speaker_get_state() │
//    └──────────────┬──────────────┘
//                   │  public API calls only
//                   ▼
//    ┌─────────────────────────────┐
//    │  speaker_driver             │  ← DRIVER LAYER (speaker_driver.c)
//    │  handles HOW to play        │
//    │                             │
//    │  network task (HTTP fetch)  │
//    │  playback task (I2S write)  │
//    │  stream buffer (decoupling) │
//    └─────────────────────────────┘
//
//  Lifecycle in main.c:
//    voice_assistant_init();   // optional future config
//    xTaskCreate(voice_assistant_task, ...);
//
// ============================================================================

/**
 * @brief Initialize the voice assistant (reserved for future config).
 *
 * Currently a no-op. Will be used when mic driver, wake word engine,
 * or backend URL configuration is added.
 */
void voice_assistant_init(void);

/**
 * @brief Main voice assistant FreeRTOS task.
 *
 * Waits for network, then enters a trigger-play-wait loop:
 *   1. Wait for trigger (button press, wake word, etc.)
 *   2. Call speaker_play_url() to fetch + play audio from backend
 *   3. Wait for playback to complete (poll speaker_get_state())
 *   4. Return to step 1
 *
 * Should run at TASK_PRIORITY (same as other application tasks).
 * The speaker driver's internal tasks handle priority separation.
 */
void voice_assistant_task(void *param);

#ifdef __cplusplus
}
#endif
