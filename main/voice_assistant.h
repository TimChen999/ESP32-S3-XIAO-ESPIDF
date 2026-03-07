#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
//  VOICE ASSISTANT — Header
//
//  Application-layer task that connects the speaker driver to the backend.
//  This module does NOT own any hardware — it calls the speaker driver's
//  public API to initiate playback and monitors completion.
//
//    ┌─────────────────────────────┐
//    │  voice_assistant_task       │  APPLICATION LAYER (this file)
//    │                             │  calls speaker_play_url()
//    │                             │  polls speaker_get_state()
//    └──────────────┬──────────────┘
//                   │  public API only
//                   ▼
//    ┌─────────────────────────────┐
//    │  speaker_driver             │  DRIVER LAYER (speaker_driver.c)
//    │  HTTP fetch + I2S playback  │  handles everything internally
//    └─────────────────────────────┘
//
// ============================================================================

/**
 * @brief Initialize the voice assistant (reserved for future config).
 */
void voice_assistant_init(void);

/**
 * @brief Main voice assistant FreeRTOS task.
 *
 * Waits for network, calls speaker_play_url() to stream audio from the
 * backend, waits for playback to finish, then repeats.
 */
void voice_assistant_task(void *param);

#ifdef __cplusplus
}
#endif
