#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
//  VOICE ASSISTANT — Header
//
//  Application-layer task that coordinates the mic driver and speaker
//  driver to create a push-to-talk voice conversation loop. This module
//  does NOT own any hardware — it calls the mic and speaker driver public
//  APIs and handles the push-to-talk button.
//
//  Push-to-talk conversation flow:
//    1. Wait for button press
//    2. mic_start() → record user speech
//    3. Wait for button release
//    4. mic_stop() → stop recording
//    5. mic_upload(url) → send PCM to backend (STT → LLM → TTS)
//    6. Wait for upload complete
//    7. speaker_play_url(url) → play backend's audio response
//    8. Wait for playback complete
//    9. Loop to step 1
//
//    ┌─────────────────────────────┐
//    │  voice_assistant_task       │  APPLICATION LAYER (this file)
//    │                             │  handles button, sequences drivers
//    └──────┬──────────────┬───────┘
//           │              │  public API only
//           ▼              ▼
//    ┌──────────────┐ ┌──────────────┐
//    │ mic_driver   │ │ speaker_drv  │  DRIVER LAYER
//    │ I2S RX +     │ │ HTTP fetch + │  each handles everything
//    │ HTTP upload  │ │ I2S playback │  internally
//    └──────────────┘ └──────────────┘
//
// ============================================================================

/**
 * @brief Initialize the voice assistant.
 *
 * Configures the push-to-talk button GPIO (input with internal pull-up).
 * Call after mic_init() and speaker_init().
 */
void voice_assistant_init(void);

/**
 * @brief Main voice assistant FreeRTOS task.
 *
 * Waits for network, then enters the push-to-talk loop:
 * button press → record → upload → play response → repeat.
 */
void voice_assistant_task(void *param);

#ifdef __cplusplus
}
#endif
