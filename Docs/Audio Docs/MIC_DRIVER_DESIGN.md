# Mic Driver — Design & Implementation Guide

This document covers the complete design for adding audio input to the
ESP32-S3 XIAO. The driver captures PCM audio from a digital microphone
via I2S, buffers it, and uploads it to a backend server over HTTP. The
backend processes the audio (speech-to-text, LLM, TTS) and streams a
response back — which the speaker driver plays.

By the end of this document, you will understand the hardware options,
the software architecture, every design decision you need to make, and
how the mic driver integrates with the existing speaker driver and voice
assistant.

---

## System Overview

The mic driver is the mirror image of the speaker driver. Where the
speaker receives PCM from the network and writes to I2S (output), the
mic reads from I2S (input) and sends PCM to the network.

```
          ┌─────┐
          │ 🎤  │  Microphone (digital MEMS, I2S output)
          └──┬──┘
             │  I2S bus: BCLK, WS, DOUT (mic's data out)
             │
  ┌──────────▼───────────┐
  │    ESP32-S3 XIAO     │
  │                      │
  │  ┌────────────────┐  │
  │  │  I2S Capture   │  │   FreeRTOS Task — reads PCM samples
  │  │  Task           │  │   from I2S peripheral, writes to buffer
  │  └───────┬────────┘  │
  │          │            │
  │  ┌───────▼────────┐  │
  │  │  Ring Buffer    │  │   Thread-safe buffer decoupling
  │  │  (8–16 KB)      │  │   I2S timing from network uploads
  │  └───────┬────────┘  │
  │          │            │
  │  ┌───────▼────────┐  │
  │  │  Network Task  │  │   FreeRTOS Task — reads from buffer,
  │  │  (HTTP TX)     │  │   uploads PCM bytes to backend
  │  └───────┬────────┘  │
  │          │            │
  └──────────┼───────────┘
             │  raw PCM bytes over HTTP POST
             │
  ┌──────────▼───────────┐
  │    CLOUD BACKEND     │
  │                      │
  │  STT converts audio  │
  │  to text, LLM        │
  │  generates response  │
  └──────────────────────┘
```

The ESP32 does **no audio processing** on the mic input. Raw PCM samples
go straight from I2S to the network. The backend handles speech-to-text,
noise reduction, or any other processing. This keeps firmware lean.

---

## What Carries Over from the Speaker Driver

The mic driver mirrors the speaker driver's architecture. Most design
choices are the same, just reversed in direction.

| Design aspect | Speaker driver | Mic driver | Same? |
|---------------|---------------|------------|-------|
| File structure | `speaker_driver.c/.h` | `mic_driver.c/.h` | Pattern ✓ |
| Config struct + state enum | `speaker_config_t` | `mic_config_t` | Pattern ✓ |
| One init + one task in `main.c` | `speaker_init()` + `speaker_playback_task` | `mic_init()` + `mic_capture_task` | Pattern ✓ |
| Stream buffer for decoupling | network → buffer → I2S | I2S → buffer → network | **Reversed** |
| Two-task producer-consumer | net=producer, I2S=consumer | I2S=producer, net=consumer | **Reversed** |
| Hardware task at higher priority | playback at priority 6 | capture at priority 6 | Same ✓ |
| Ephemeral network task | spawned by `speaker_play_url()` | spawned by `mic_record_and_send()` | Pattern ✓ |
| I2S API | `i2s_channel_write()` (TX) | `i2s_channel_read()` (RX) | Different call |
| HTTP direction | HTTP response → buffer | buffer → HTTP request body | **Reversed** |
| Volume/gain scaling | software volume on output | software gain on input (optional) | Same concept |
| `SIMULATE` flag for Wokwi | `SPEAKER_SIMULATE` | `MIC_SIMULATE` | Pattern ✓ |
| Audio format configurable | compile-time `#define` presets | compile-time `#define` presets | Same ✓ |
| Pin assignment configurable | `I2S_BCLK/WS/DOUT_PIN` | `I2S_DIN_PIN` (+ possibly BCLK/WS) | Same approach |
| State machine | IDLE→BUFFERING→PLAYING→DRAINING→STOPPED | IDLE→RECORDING→UPLOADING→DONE | Similar |

**Key difference:** The speaker driver's two tasks exist because network
delivers data in bursts but I2S demands steady data. The mic driver has
the same problem in reverse: I2S delivers data at a steady rate but
network uploads happen in bursts. The buffer + two-task split is required
for exactly the same reason.

```
  Speaker problem:
    Network (bursty) ──► buffer ──► I2S (steady)

  Mic problem (mirror):
    I2S (steady) ──► buffer ──► Network (bursty)

  Same solution: buffer + priority separation.
```

---

## Design Decisions

### Decision 1: Microphone Type

The microphone converts sound waves into a digital signal. Digital MEMS
microphones output I2S directly — no ADC needed.

| Option | Interface | Quality | Cost | Complexity |
|--------|-----------|---------|------|------------|
| **INMP441** | I2S standard | Good (SNR 61dB) | ~$2 | Low — same I2S protocol as speaker |
| **SPH0645** | I2S standard | Good (SNR 65dB) | ~$3 | Low — same as INMP441 |
| **MSM261S4030H0** | PDM | Good (SNR 61dB) | Built-in on XIAO Sense | Different mode — `i2s_pdm_rx` |
| **ICS-43434** | I2S standard | Very Good (SNR 70dB) | ~$5 | Low |
| **Analog mic + ADC** | ADC pin | Fair | ~$1 | Medium — needs ADC sampling code |

**Recommendation:** Use **INMP441** if adding an external mic. It speaks
standard I2S (same protocol as the MAX98357A speaker amp), so the init
code is very similar. If using the **XIAO ESP32-S3 Sense** board, the
built-in PDM mic (MSM261S4030H0) is already wired — but needs PDM mode
instead of standard I2S.

**Impact on driver code:**

```
                  INMP441 (I2S standard)        MSM261S4030H0 (PDM)
                  ──────────────────────        ───────────────────
  Init:           i2s_channel_init_std_mode()   i2s_channel_init_pdm_rx_mode()
                  3 pins: BCLK, WS, DIN         2 pins: CLK, DATA
                  Same Philips mode as speaker   Different config struct

  Read:           i2s_channel_read()             i2s_channel_read()
                  (same call either way)         (same call either way)

  Data format:    16-bit PCM samples             PDM→PCM conversion done
                  directly from mic              by I2S hardware (decimation)

  Sharing with    Can share BCLK+WS with         Cannot share with speaker
  speaker:        speaker (full-duplex)           (different protocol entirely)
```

Design decision: Design the software for standard I2C interface

### Decision 2: I2S Peripheral — Share with Speaker or Separate

This is the coordination point between the mic and speaker drivers. The
ESP32-S3 has two I2S peripherals (I2S0 and I2S1).

| Option | Pins used for mic | Code coordination | Pros | Cons |
|--------|-------------------|-------------------|------|------|
| **Full-duplex I2S0** | 1 new pin (DIN only) | `i2s_new_channel()` must create both TX+RX handles | Saves 2 GPIO pins (shares BCLK+WS) | Speaker and mic init must coordinate |
| **Separate I2S1** | 3 new pins (BCLK, WS, DIN) | Completely independent | Zero coordination, simplest code | Uses 3 extra GPIO pins |

**Full-duplex detail:** The speaker driver currently calls
`i2s_new_channel(&cfg, &tx_handle, NULL)` — the NULL means "no RX."
For full-duplex, this becomes `i2s_new_channel(&cfg, &tx_handle, &rx_handle)`,
creating both handles at once. Then `tx_handle` goes to the speaker driver
and `rx_handle` goes to the mic driver. After that one coordination point,
both drivers operate completely independently.

**Separate peripheral detail:** The mic driver calls
`i2s_new_channel(&cfg, NULL, &rx_handle)` on `I2S_NUM_1`. No changes to
the speaker driver at all. The mic has its own BCLK and WS clocks.

**Making it configurable:** The mic driver can define which I2S peripheral
to use. If set to `I2S_NUM_0`, it operates in full-duplex mode and expects
the speaker driver to provide the RX handle. If set to `I2S_NUM_1`, it
creates its own channel independently.

```
  Full-duplex (I2S0, shared clocks):

    ESP32-S3                         Devices
    ────────                         ───────
    BCLK (GPIO9)  ────────────────► MAX98357A BCLK
                              └───► INMP441 BCLK

    WS   (GPIO10) ────────────────► MAX98357A LRC
                              └───► INMP441 WS

    DOUT (GPIO44) ────────────────► MAX98357A DIN    (ESP → speaker)
    DIN  (new pin)◄──────────────── INMP441 SD       (mic → ESP)


  Separate (I2S0 for speaker, I2S1 for mic):

    ESP32-S3                         Devices
    ────────                         ───────
    I2S0 BCLK (GPIO9)  ─────────── MAX98357A BCLK
    I2S0 WS   (GPIO10) ─────────── MAX98357A LRC
    I2S0 DOUT (GPIO44) ─────────── MAX98357A DIN

    I2S1 BCLK (new pin) ──────────► INMP441 BCLK
    I2S1 WS   (new pin) ──────────► INMP441 WS
    I2S1 DIN  (new pin) ◄────────── INMP441 SD
```

Design decision: Leave configurable — define `MIC_I2S_NUM` as `I2S_NUM_0`
(full-duplex) or `I2S_NUM_1` (separate). Default to whichever you pick,
with comments explaining the trade-off. Explain with comments on the code how it works, how to configure.

### Decision 3: Audio Format (Sample Rate, Bit Depth, Channels)

The mic capture format determines I2S RX configuration and upload bandwidth.
This should match (or be compatible with) the backend's STT input requirements.

| Parameter | Option A (Speech/STT) | Option B (High Quality) |
|-----------|----------------------|------------------------|
| Sample rate | 16,000 Hz | 48,000 Hz |
| Bit depth | 16-bit | 16-bit |
| Channels | Mono (1 ch) | Mono (1 ch) |
| Bandwidth | 32 KB/s | 96 KB/s |
| Quality | Good for speech recognition | Exceeds STT requirements |
| Use case | LLM voice input (Whisper, etc.) | High-fidelity recording |

**Recommendation:** Use **16 kHz / 16-bit / mono** — matches the speaker
format and is the native input format for most STT APIs (OpenAI Whisper,
Google Speech-to-Text, Deepgram). Higher sample rates waste bandwidth
without improving recognition accuracy.

**Bandwidth calculation:**

```
  Bytes/sec = sample_rate × bit_depth/8 × channels

  16 kHz mono:  16000 × 2 × 1 = 32,000 B/s  = 32 KB/s = 256 kbps
  48 kHz mono:  48000 × 2 × 1 = 96,000 B/s  = 96 KB/s = 768 kbps
```

Design decision: Configurable via compile-time `#define` presets (same
approach as speaker). Default match speaker, comments show what configs set/do

### Decision 4: Ring Buffer Size

Same concept as the speaker buffer, but in reverse. The buffer sits
between the I2S capture task and the network upload task. It absorbs
upload jitter so the I2S capture never stalls.

| Buffer Size | Holds (at 16kHz mono) | Pros | Cons |
|-------------|----------------------|------|------|
| 4 KB | 125 ms | Minimal RAM | Overrun risk if upload stalls |
| **8 KB** | 250 ms | Good balance | Handles brief upload delays |
| 16 KB | 500 ms | Generous | More latency before upload starts |

**Recommendation:** **8 KB** ring buffer. Mic capture is lower priority
than speaker playback for user experience — a brief lost sample during
upload stall is less noticeable than a speaker click. 8 KB saves RAM
compared to the speaker's 16 KB.

Design decision: Should be configurable, default values match speaker, comment show what configs set/do

### Decision 5: Trigger Mechanism — Push to Talk

The mic driver needs a way to know when to start and stop recording.

| Trigger | How | Pros | Cons |
|---------|-----|------|------|
| **Push-to-talk (GPIO button)** | Hold button to record, release to stop | Simple, explicit user intent | Requires physical button |
| **Push-to-start, push-to-stop** | Toggle recording with button press | One-handed operation | Need to handle double-press edge cases |
| **Voice Activity Detection (VAD)** | Detect speech vs silence in PCM | Hands-free | CPU cost, complexity, false triggers |
| **Fixed duration** | Record for N seconds after trigger | Simplest | May cut off mid-sentence or waste time |

**Recommendation:** Start with **push-to-talk** (hold to record). It's
the simplest to implement and gives the user explicit control. The voice
assistant task handles the button — the mic driver itself just exposes
`mic_start()` and `mic_stop()`.

```
  Push-to-talk flow:

  User presses button ─────► voice_assistant detects GPIO
                              │
                              ├── mic_start()
                              │     → I2S RX enabled
                              │     → capture task begins reading
                              │
  User holds button           │   mic recording...
  (audio captured)            │   I2S → buffer → (held until upload)
                              │
  User releases button ──────► voice_assistant detects GPIO release
                              │
                              ├── mic_stop()
                              │     → I2S RX disabled
                              │     → capture complete
                              │
                              ├── mic_upload(url)
                              │     → uploads captured PCM to backend
                              │     → backend processes: STT → LLM → TTS
                              │
                              ├── speaker_play_url(url)
                              │     → plays backend's audio response
                              │
                              └── wait for IDLE, loop
```

Design decision: Use a push to talk trigger, make it modular and easily swappable. ALso use comments to explain how it interfaces with the system, how it works

### Decision 6: I2S Pin Assignment

If using full-duplex (I2S0 shared with speaker), only one new pin is
needed: DIN (mic data input). BCLK and WS are shared with the speaker.

If using separate I2S1, three new pins are needed.

**Pins already in use:**

| Pin | Function |
|-----|----------|
| GPIO1–8 | Modem UART (D0–D5, D8–D9) — free in WiFi mode |
| GPIO9 | Speaker I2S BCLK (D10) |
| GPIO10 | Speaker I2S WS (D7) |
| GPIO44 | Speaker I2S DOUT (D6) |

**Available for mic (full-duplex, 1 pin needed):**

| Pin | XIAO Label | Suggested Function |
|-----|-----------|-------------------|
| GPIO1 | D0 | I2S DIN (mic data in) — free in WiFi mode |
| GPIO2 | D1 | Alternative DIN |

**Available for mic (separate I2S1, 3 pins needed):**

| Pin | XIAO Label | Suggested Function |
|-----|-----------|-------------------|
| GPIO1 | D0 | I2S1 BCLK |
| GPIO2 | D1 | I2S1 WS |
| GPIO3 | D2 | I2S1 DIN |

**Button pin for push-to-talk:**

Any remaining GPIO with internal pull-up. Connect button between the pin
and GND. Configure as `GPIO_MODE_INPUT` with `GPIO_PULLUP_ENABLE`.

| Pin | XIAO Label | Notes |
|-----|-----------|-------|
| GPIO4 | D3 | Good candidate if modem pins are free |

Design decision: give a good default for whatever pins you need and they should be configurable (pick pins — configurable via `#define`) Use comments to indicate what the pins set/how they interface with the system

### Decision 7: Software Gain Control

Same concept as speaker volume scaling, but for input. Some MEMS mics
have low output levels and may benefit from amplification before upload.

| Strategy | How | Pros | Cons |
|----------|-----|------|------|
| **No gain** | Send raw PCM as-is | Simplest | May be too quiet for STT |
| **Software gain** | Multiply samples by gain factor (1.0–4.0) | No hardware | Amplifies noise too, possible clipping |
| **AGC (auto gain)** | Track signal level, adjust gain dynamically | Consistent volume | More complex, CPU cost |

**Recommendation:** Start with **no gain** or a simple fixed gain
multiplier. Most STT APIs handle varying input levels well. Add AGC
only if recognition accuracy suffers.

Design decision: Use a configurable gain amount. Make it modular so it should be easily swapped with a more advanced form of volume control. Point this out with comments (what this sets, how this gain system can be changed)

---

## Driver Architecture

### File Structure

```
  main/
  ├── main.c                (modified — add mic init + task creation)
  ├── mic_driver.c           (new — I2S RX + ring buffer + capture logic)
  ├── mic_driver.h           (new — public API + config struct)
  ├── voice_assistant.c      (modified — add push-to-talk + mic integration)
  ├── voice_assistant.h      (modified — if new API needed)
  ├── speaker_driver.c       (may need one-line change for full-duplex)
  ├── speaker_driver.h       (unchanged)
  └── ...
```

### Config Struct

```c
typedef enum {
    MIC_STATE_IDLE,         // initialized, not recording
    MIC_STATE_RECORDING,    // I2S RX active, capturing to buffer
    MIC_STATE_UPLOADING,    // capture done, sending buffer to backend
    MIC_STATE_DONE,         // upload complete
    MIC_STATE_ERROR,
} mic_state_t;

typedef struct {
    int bclk_pin;            // I2S bit clock GPIO (shared or own)
    int ws_pin;              // I2S word select GPIO (shared or own)
    int din_pin;             // I2S data in GPIO (mic's data out)
    int button_pin;          // Push-to-talk GPIO (-1 if not used)
    uint32_t sample_rate;    // e.g. 16000
    uint8_t bits_per_sample; // 16
    uint8_t channels;        // 1 = mono
    size_t ring_buffer_size; // in bytes (e.g. 8192)
    float gain;              // 1.0 = unity, >1.0 = amplify
    mic_state_t state;
} mic_config_t;
```

### Public API

```c
/* Initialize I2S RX peripheral and allocate ring buffer. */
void mic_init(void);

/* Start recording — enables I2S RX, capture task begins filling buffer. */
void mic_start(void);

/* Stop recording — disables I2S RX. */
void mic_stop(void);

/* Upload captured audio to the given URL as HTTP POST body.
   Spawns an internal network task, returns immediately. */
void mic_upload(const char *url);

/* Query current state. */
mic_state_t mic_get_state(void);

/* FreeRTOS task entry point for I2S capture (internal). */
void mic_capture_task(void *param);
```

### State Machine

```
                        mic_init()
                             │
                             ▼
                      ┌─────────────┐
                ┌────►│    IDLE      │◄──────────────────────────┐
                │     └──────┬──────┘                            │
                │            │ mic_start()                       │
                │            │   (voice_assistant — button press) │
                │            ▼                                   │
                │     ┌─────────────┐                            │
                │     │  RECORDING  │                            │
                │     │             │                            │
                │     │ I2S RX reads│                            │
                │     │ mic samples │                            │
                │     │ into buffer │                            │
                │     └──────┬──────┘                            │
                │            │ mic_stop()                        │
                │            │   (voice_assistant — button release)
                │            ▼                                   │
                │     ┌─────────────┐                            │
                │     │  UPLOADING  │                            │
                │     │             │                            │
                │     │ HTTP POST   │                            │
                │     │ sends buffer│                            │
                │     │ to backend  │                            │
                │     └──────┬──────┘                            │
                │            │ upload complete                   │
                │            ▼                                   │
                │     ┌─────────────┐                            │
                │     │    DONE     │────────────────────────────┘
                │     └─────────────┘   (auto-return to IDLE)
                │
                │  mic_stop() from any state → IDLE
                └──────────────────────────

           ERROR ◄── I2S read failure, HTTP upload failure
```

---

## Task Architecture

Two FreeRTOS tasks, same producer-consumer pattern as the speaker but
reversed:

```
  ┌─────────────────────────────┐       ┌─────────────────────────────┐
  │   CAPTURE TASK              │       │   UPLOAD TASK               │
  │   (producer)                │       │   (consumer)                │
  │                             │       │                             │
  │   1. Wait for RECORDING     │       │   1. Wait for UPLOADING     │
  │   2. i2s_channel_read()     │ ring  │   2. Read N bytes from      │
  │      → gets mic samples  ───┼──────►│      ring buffer            │
  │   3. Apply gain (optional)  │buffer │   3. Write to HTTP POST     │
  │   4. Write to ring buffer   │       │      body (chunked)         │
  │   5. Loop until stopped     │       │   4. Loop until buffer empty│
  │                             │       │   5. Close HTTP, DONE       │
  └─────────────────────────────┘       └─────────────────────────────┘
        │                                       │
        │  Priority: TASK_PRIORITY + 1          │  Priority: TASK_PRIORITY
        │  (I2S hardware can't wait)            │  (network can wait)
        │                                       │
        │  Blocks on: i2s_channel_read()        │  Blocks on: ring buffer empty
        │                                       │             HTTP write
        └───────────────────────────────────────┘
```

**Why the capture task has higher priority:** Same reason as the speaker.
If I2S RX overflows (capture task too slow), mic samples are lost — the
recording has gaps. A network upload can wait 10ms without consequence.

---

## Voice Assistant Integration

The voice assistant task becomes the conversation coordinator. It manages
the push-to-talk button, sequences the mic and speaker drivers, and
connects both to the backend.

### Modified Voice Assistant Loop

```
  voice_assistant_task (with mic + speaker):

  1. Wait for network
  2. Wait for button press           ← GPIO interrupt or polling
  3. mic_start()                     ← I2S RX on, recording starts
  4. Wait for button release
  5. mic_stop()                      ← I2S RX off, recording stops
  6. mic_upload(BACKEND_URL)         ← sends recorded PCM to backend
  7. Wait for mic DONE
  8. speaker_play_url(BACKEND_URL)   ← backend streams response audio
  9. Wait for speaker IDLE
  10. Loop back to step 2
```

### Interaction Diagram

```
  User        voice_assistant     mic_driver        speaker_driver    backend
  ────        ───────────────     ──────────        ──────────────    ───────

  press ─────► button detected
               │
               ├── mic_start() ──► I2S RX on
               │                   recording...
  hold         │                   I2S → buffer
               │                   ...
  release ────► button release
               │
               ├── mic_stop() ───► I2S RX off
               │
               ├── mic_upload() ─► HTTP POST ──────────────────────► received
               │                   buffer → body                      │
               │                   ...                                │ STT
               │    mic DONE ◄──── upload complete                    │ LLM
               │                                                      │ TTS
               ├── speaker_play_url()                                 │
               │    └──► network task                                 │
               │         HTTP POST ──────────────────────────────────►│
               │         recv() BLOCKS                                │
               │         ...                                          ▼
               │         ◄─────────────────────── chunk: [PCM bytes]
               │         buffer fills
               │         playback task → I2S → speaker
               │         ...
               │         ◄─────────────────────── chunk: 0 (end)
               │         DRAINING → STOPPED → IDLE
               │
               │    speaker IDLE
               │
               └── loop (wait for next button press)
```

### Button Handling

The button is handled by the voice assistant, NOT the mic driver. This
keeps the mic driver hardware-agnostic (it just exposes start/stop).

```c
// In voice_assistant.c:

#define BUTTON_PIN  4  // push-to-talk GPIO

// Simple polling approach (can be upgraded to GPIO interrupt later):
static bool button_pressed(void) {
    return gpio_get_level(BUTTON_PIN) == 0;  // active low with pull-up
}

// In the task loop:
while (!button_pressed()) {
    vTaskDelay(pdMS_TO_TICKS(20));
}
mic_start();

while (button_pressed()) {
    vTaskDelay(pdMS_TO_TICKS(20));
}
mic_stop();
```

---

## I2S Peripheral Configuration

### Full-Duplex on I2S0 (shared with speaker)

If `MIC_I2S_NUM` is `I2S_NUM_0`, the speaker and mic share one peripheral.
The `i2s_new_channel()` call must create both handles. This requires a
thin coordination layer:

```
  Option A — Speaker creates both handles:

    // In speaker_init() — modified:
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle);
    //                                       ^^^^^^^^^^^ new

    // Expose RX handle for mic driver:
    i2s_chan_handle_t speaker_get_rx_handle(void);

    // In mic_init():
    s_rx_handle = speaker_get_rx_handle();


  Option B — Shared audio_init() in main.c:

    // In main.c:
    i2s_chan_handle_t tx_handle, rx_handle;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle);

    speaker_init_with_handle(tx_handle);
    mic_init_with_handle(rx_handle);
```

### Separate I2S1 (independent)

If `MIC_I2S_NUM` is `I2S_NUM_1`, no coordination needed:

```c
// In mic_init() — completely independent:
i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
    I2S_NUM_1, I2S_ROLE_MASTER);
i2s_new_channel(&chan_cfg, NULL, &s_rx_handle);  // RX only
```

### I2S RX Standard Mode Config

```c
i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(cfg->sample_rate),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                    I2S_DATA_BIT_WIDTH_16BIT,
                    I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = (gpio_num_t)cfg->bclk_pin,
        .ws   = (gpio_num_t)cfg->ws_pin,
        .dout = I2S_GPIO_UNUSED,          // no speaker on this channel
        .din  = (gpio_num_t)cfg->din_pin, // mic data in
        .invert_flags = { false, false, false },
    },
};
i2s_channel_init_std_mode(s_rx_handle, &std_cfg);
```

---

## Network Upload Design

### Upload Flow

```
  ESP32                                Backend
  ─────                                ───────

  mic_upload(url) called
       │
       ├──► POST /api/listen
       │    Headers:
       │      Content-Type: application/octet-stream
       │      X-Audio-Format: pcm;rate=16000;bits=16;channels=1
       │      Content-Length: <total bytes captured>
       │
       │    Body: [raw PCM bytes from ring buffer]
       │
       │                               Backend receives PCM
       │                                 ├── STT (speech-to-text)
       │                                 ├── LLM (generate response)
       │                                 ├── TTS (text-to-speech)
       │                                 └── Transcode to PCM
       │
       │    ◄── HTTP 200 OK
       │        (response handled by speaker_play_url separately)
       │
  mic state = DONE
```

Unlike the speaker driver which streams in real-time (chunked response),
the mic upload sends a complete recording as the request body. The
recording is fully captured before upload begins (push-to-talk defines
start and end). This is simpler than streaming mic audio in real-time.

---

## Implementation Plan — Step by Step

### Step 1: I2S RX Smoke Test — Read Mic Samples

**Goal:** Verify I2S RX wiring and mic by reading and logging raw samples.

| What to implement | How to verify |
|-------------------|---------------|
| `mic_init()` — configure I2S RX channel | No crash |
| Read raw samples with `i2s_channel_read()` in a loop | Log sample values |
| Check sample values are non-zero and vary with sound | Tap mic, see values change |

### Step 2: Ring Buffer + Capture Task

**Goal:** Decouple I2S reads from processing.

| What to implement | How to verify |
|-------------------|---------------|
| Create `StreamBufferHandle_t` in `mic_init()` | No crash |
| `mic_capture_task()` — reads I2S, writes to buffer | Buffer fills when recording |
| `mic_start()` / `mic_stop()` control recording | Start/stop works cleanly |

### Step 3: HTTP Upload

**Goal:** Send captured PCM to backend.

| What to implement | How to verify |
|-------------------|---------------|
| `mic_upload(url)` — reads buffer, sends as HTTP POST body | Backend receives PCM |
| State machine: RECORDING → UPLOADING → DONE | States transition correctly |
| Backend can play back the received audio | Audio sounds correct |

### Step 4: Voice Assistant Integration

**Goal:** Push-to-talk conversation loop.

| What to implement | How to verify |
|-------------------|---------------|
| Button GPIO init in `voice_assistant_init()` | Button reads correctly |
| Modified task loop: button → mic → upload → speaker | Full conversation works |

### Step 5: I2S Peripheral Sharing (if full-duplex)

**Goal:** Speaker and mic share I2S0.

| What to implement | How to verify |
|-------------------|---------------|
| Modify `i2s_new_channel()` to create TX+RX handles | Both channels work |
| Pass RX handle to mic driver | Mic captures while speaker is idle |
| Test: record → upload → playback (no conflicts) | Clean audio both directions |

---

## Memory Budget

| Component | RAM Usage | Notes |
|-----------|----------|-------|
| Ring buffer | 8,192 bytes | Configurable via `ring_buffer_size` |
| I2S DMA buffers | ~4,096 bytes | Managed by I2S driver |
| Capture task stack | 4,096 bytes | Standard task stack |
| Upload task stack | 4,096 bytes | HTTP client needs stack |
| I2S RX channel handle | ~200 bytes | Internal ESP-IDF |
| **Mic driver total** | **~21 KB** | ~4% of ESP32-S3's 512KB SRAM |
| **+ Speaker driver** | **~29 KB** | Already allocated |
| **Combined total** | **~50 KB** | ~10% of SRAM |

---

## Appendix: Comparison of Speaker and Mic Architectures

| Aspect | Speaker Driver | Mic Driver |
|--------|---------------|------------|
| Peripheral | I2S TX | I2S RX |
| Data direction | network → I2S (output) | I2S → network (input) |
| Config struct | `speaker_config_t` | `mic_config_t` |
| State machine | 5 states (IDLE → PLAYING → STOPPED) | 4 states (IDLE → RECORDING → DONE) |
| Number of tasks | 2 (network producer + I2S consumer) | 2 (I2S producer + network consumer) |
| Buffer purpose | Absorb network jitter | Absorb upload jitter |
| Higher-priority task | Playback (I2S TX must not starve) | Capture (I2S RX must not overflow) |
| Network role | Downloads PCM (HTTP response) | Uploads PCM (HTTP request body) |
| Trigger | `speaker_play_url()` — called by voice assistant | `mic_start()` / `mic_stop()` — called by voice assistant |
| Real-time streaming | Yes (plays while downloading) | No (captures fully, then uploads) |
