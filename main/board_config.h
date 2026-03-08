/*
 * ============================================================================
 * BOARD CONFIGURATION — Centralised Pin & Peripheral Assignments
 * ============================================================================
 *
 * All board-specific GPIO and peripheral mappings live in this single file.
 * Each driver #includes this header instead of defining its own pins.
 *
 * To switch boards:
 *   1. Change BOARD_TARGET to the new board macro.
 *   2. Rebuild.  All drivers pick up the new pin map automatically.
 *
 * To add a new board:
 *   1. Add a BOARD_<NAME> macro with a unique integer.
 *   2. Add an #elif block for it inside every pin section below.
 *
 * ============================================================================
 */

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

// ============================================================================
//  BOARD SELECTION
//
//  Uncomment / set BOARD_TARGET to ONE of the boards below.
// ============================================================================
#define BOARD_ESP32S3_XIAO          0
#define BOARD_ESP32S3_DEVKITC_1     1

#define BOARD_TARGET                BOARD_ESP32S3_XIAO

// ============================================================================
//  PUSH-TO-TALK BUTTON PIN
//
//  GPIO for the momentary push-to-talk button.
//  Any GPIO with internal pull-up capability works.
//  Wire: GPIO ←→ button ←→ GND (internal pull-up enabled in firmware).
//
//  Used by: voice_assistant.c
// ============================================================================
#if BOARD_TARGET == BOARD_ESP32S3_XIAO
#define PTT_BUTTON_PIN              4       // D3 on XIAO

#elif BOARD_TARGET == BOARD_ESP32S3_DEVKITC_1
#define PTT_BUTTON_PIN              4       // GPIO4 on DevKitC-1 header

#endif

// ============================================================================
//  I2S MICROPHONE — Peripheral & Pin Assignments
//
//  MIC_I2S_NUM selects which I2S peripheral the mic uses:
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
//  For the INMP441 mic, wire: BCLK→SCK, WS→WS, DIN→SD, VDD→3.3V, GND→GND.
//
//  Used by: mic_driver.c
// ============================================================================
#define MIC_I2S_NUM                 1

#if BOARD_TARGET == BOARD_ESP32S3_XIAO

#if MIC_I2S_NUM == 0
#define MIC_I2S_BCLK_PIN           9       // D10/SDA — shared with speaker
#define MIC_I2S_WS_PIN             10      // D7/SCL  — shared with speaker
#define MIC_I2S_DIN_PIN            1       // D0
#else
#define MIC_I2S_BCLK_PIN           1       // D0
#define MIC_I2S_WS_PIN             2       // D1
#define MIC_I2S_DIN_PIN            3       // D2
#endif

#elif BOARD_TARGET == BOARD_ESP32S3_DEVKITC_1

#if MIC_I2S_NUM == 0
#define MIC_I2S_BCLK_PIN           9       // shared with speaker
#define MIC_I2S_WS_PIN             10      // shared with speaker
#define MIC_I2S_DIN_PIN            1
#else
#define MIC_I2S_BCLK_PIN           1
#define MIC_I2S_WS_PIN             2
#define MIC_I2S_DIN_PIN            3
#endif

#endif

// ============================================================================
//  I2S SPEAKER (AMPLIFIER) — Pin Assignments
//
//  GPIO pins for the I2S bus connecting to the amplifier (e.g.
//  MAX98357A, NS4168). No I2C pins are needed for I2S-only amps.
//
//  In WiFi-only mode (modem pins free), any GPIO can be used.
//  If using a codec chip with I2C, reassign I2S pins and keep the
//  default I2C bus pins (SDA/SCL) free.
//
//  Used by: speaker_driver.c
// ============================================================================
#if BOARD_TARGET == BOARD_ESP32S3_XIAO
#define SPEAKER_I2S_BCLK_PIN       9       // D10/SDA
#define SPEAKER_I2S_WS_PIN         10      // D7/SCL
#define SPEAKER_I2S_DOUT_PIN       44      // D6

#elif BOARD_TARGET == BOARD_ESP32S3_DEVKITC_1
#define SPEAKER_I2S_BCLK_PIN       9
#define SPEAKER_I2S_WS_PIN         10
#define SPEAKER_I2S_DOUT_PIN       44

#endif

// ============================================================================
//  MODEM DRIVER UART — Pin Assignments
//
//  UART connecting the driver side to the modem.
//
//  Physical wiring (see diagram.json):
//    Driver TX  ───green wire──► Modem RX
//    Modem TX   ───blue wire───► Driver RX
//
//  Flow control wiring (optional — set FLOW_CONTROL_ENABLED in modem_driver.c):
//    Driver RTS ───yellow wire─► Modem CTS
//    Modem RTS  ───orange wire─► Driver CTS
//
//  Used by: modem_driver.c
// ============================================================================
#if BOARD_TARGET == BOARD_ESP32S3_XIAO
#define DRIVER_UART_NUM             1
#define DRIVER_TX_PIN               1       // D0
#define DRIVER_RX_PIN               2       // D1
#define DRIVER_RTS_PIN              3       // D2 (flow control)
#define DRIVER_CTS_PIN              4       // D3 (flow control)

#elif BOARD_TARGET == BOARD_ESP32S3_DEVKITC_1
#define DRIVER_UART_NUM             1
#define DRIVER_TX_PIN               1
#define DRIVER_RX_PIN               2
#define DRIVER_RTS_PIN              3
#define DRIVER_CTS_PIN              4

#endif

// ============================================================================
//  SIMULATED MODEM UART — Pin Assignments
//
//  UART for the simulated modem side (loopback testing on same chip).
//
//  Physical wiring (see diagram.json):
//    Modem TX  ───blue wire───► Driver RX
//    Driver TX ───green wire──► Modem RX
//
//  Flow control wiring (optional — set FLOW_CONTROL_ENABLED in sim_modem.c):
//    Modem RTS ───orange wire─► Driver CTS
//    Driver RTS───yellow wire─► Modem CTS
//
//  Used by: sim_modem.c
// ============================================================================
#if BOARD_TARGET == BOARD_ESP32S3_XIAO
#define MODEM_SIM_UART_NUM          2
#define MODEM_SIM_TX_PIN            6       // D5
#define MODEM_SIM_RX_PIN            7       // D8
#define MODEM_SIM_RTS_PIN           5       // D4 (flow control)
#define MODEM_SIM_CTS_PIN           8       // D9 (flow control)

#elif BOARD_TARGET == BOARD_ESP32S3_DEVKITC_1
#define MODEM_SIM_UART_NUM          2
#define MODEM_SIM_TX_PIN            6
#define MODEM_SIM_RX_PIN            7
#define MODEM_SIM_RTS_PIN           5
#define MODEM_SIM_CTS_PIN           8

#endif

// ============================================================================
//  SIMULATION MODE — Wokwi Audio Bridge
//
//  When set to 1, the mic and speaker drivers route I2S audio through a
//  Python HTTP bridge on the host PC instead of using real I2S hardware.
//  This lets you have a real voice conversation through the Wokwi simulator.
//
//  Set both to 0 when building for real hardware with physical I2S
//  mic (e.g. INMP441) and speaker amp (e.g. MAX98357A).
//
//  Used by: mic_driver.c, speaker_driver.c
// ============================================================================
#define MIC_SIMULATE            1
#define SPEAKER_SIMULATE        1

// ============================================================================
//  WIFI CREDENTIALS
//
//  SSID and password for the Wi-Fi network the ESP32 connects to.
//
//  Wokwi:     "Wokwi-GUEST" with an empty password (open network).
//  Hardware:  your real Wi-Fi SSID and password.
//
//  Used by: wifi_driver.c
// ============================================================================
#define WIFI_SSID               "Wokwi-GUEST"
#define WIFI_PASS               ""

// ============================================================================
//  BACKEND URLS — Voice Assistant Endpoint
//
//  BACKEND_MIC_URL: endpoint that receives recorded PCM audio via HTTP POST.
//    The backend runs STT → LLM → TTS and stores the response PCM.
//
//  BACKEND_SPEAKER_URL: endpoint that returns the TTS audio response as
//    raw PCM. The speaker driver streams and plays this.
//
//  Both URLs can point to the same endpoint (single-endpoint mode).
//  The backend distinguishes mic upload vs speaker fetch by checking
//  whether the request body is empty.
//
//  Wokwi:     use host.wokwi.internal (resolves to the host PC)
//  Hardware:  use the backend machine's LAN IP (e.g. 192.168.1.100)
//
//  Used by: voice_assistant.c
// ============================================================================
#define BACKEND_MIC_URL         "http://host.wokwi.internal:5000/api/conversation"
#define BACKEND_SPEAKER_URL     "http://host.wokwi.internal:5000/api/conversation"

// ============================================================================
//  AUDIO BRIDGE URLS — Wokwi Simulation Only
//
//  When MIC_SIMULATE=1 or SPEAKER_SIMULATE=1, the mic/speaker drivers
//  route I2S audio through a Python audio bridge running on the host PC
//  instead of using real I2S hardware.
//
//  The bridge captures PC mic audio and plays back through PC speakers.
//  See tools/audio_bridge/audio_bridge.py for the bridge server.
//
//  These are only used during Wokwi simulation. On real hardware
//  (SIMULATE=0), these defines are ignored entirely.
//
//  Used by: mic_driver.c, speaker_driver.c
// ============================================================================
#define AUDIO_BRIDGE_MIC_URL      "http://host.wokwi.internal:8080/mic"
#define AUDIO_BRIDGE_SPEAKER_URL  "http://host.wokwi.internal:8080/speaker"

#endif /* BOARD_CONFIG_H */
