# Speaker Driver — Design & Implementation Guide

This document covers the complete design for adding audio output to the
ESP32-S3 XIAO. The driver receives PCM audio data over the network (from a
backend that interfaces with an LLM/TTS API) and plays it through an I2S
amplifier/speaker. By the end of this document, you will understand the
hardware options, the software architecture, every design decision you need
to make, and the step-by-step implementation order.

---

## System Overview

The speaker driver sits at the end of an audio pipeline that starts in the
cloud and ends at a physical speaker cone:

```
  ┌──────────────────────┐
  │    CLOUD BACKEND     │
  │  (your server / API) │
  │                      │
  │  LLM generates text  │
  │  TTS converts to     │
  │  audio, transcodes   │
  │  to raw PCM          │
  └──────────┬───────────┘
             │  raw PCM bytes over HTTPS / WebSocket
             │  (streamed via chunked transfer or WS frames)
             │
  ┌──────────▼───────────┐
  │    ESP32-S3 XIAO     │
  │                      │
  │  ┌────────────────┐  │
  │  │  Network Task  │  │   FreeRTOS Task — receives audio bytes
  │  │  (HTTP/WS RX)  │  │   from network, writes to ring buffer
  │  └───────┬────────┘  │
  │          │            │
  │  ┌───────▼────────┐  │
  │  │  Ring Buffer    │  │   Thread-safe buffer decoupling
  │  │  (8–32 KB)      │  │   network jitter from audio playback
  │  └───────┬────────┘  │
  │          │            │
  │  ┌───────▼────────┐  │
  │  │  I2S Playback  │  │   FreeRTOS Task — reads from ring buffer,
  │  │  Task           │  │   writes PCM samples to I2S peripheral
  │  └───────┬────────┘  │
  │          │ I2S bus    │
  └──────────┼───────────┘
             │  BCLK, WS (LRCLK), DOUT
             │
  ┌──────────▼───────────┐
  │   I2S AMPLIFIER      │
  │   (e.g. MAX98357)    │
  │                      │
  │   Digital-to-analog  │
  │   + power amp        │
  └──────────┬───────────┘
             │  analog audio
             │
          ┌──▼──┐
          │ 🔊  │  Speaker (4Ω or 8Ω, 2–3W)
          └─────┘
```

The ESP32 does **no audio decoding**. The backend handles all format
conversion. The ESP32's job is simple: receive bytes, buffer them, write
them to I2S hardware. This keeps the firmware lean and the CPU free for
networking and other tasks.

---

## Design Decisions

Before writing any code, you need to make six decisions. Each one is
independent — pick the option that fits your project and move on.

### Decision 1: Amplifier / DAC Chip

The amplifier chip sits between the ESP32's I2S output pins and the speaker.
It converts digital I2S data to an analog signal and amplifies it.

| Option | I2C Config Required | Audio Quality | Cost | Complexity |
|--------|-------------------|---------------|------|------------|
| **MAX98357A** | No — I2S only | Good (Class D, 3.2W) | ~$3 | Lowest |
| **NS4168** | No — I2S only | Good (Class D, 3W) | ~$1 | Lowest |
| **PCM5102A** | No — I2S only | Excellent (Hi-Fi DAC) | ~$5 | Low |
| **ES8311** | Yes — I2C setup | Very Good (codec) | ~$2 | Medium |
| **WM8960** | Yes — I2C setup | Excellent (codec) | ~$4 | Higher |

**Recommendation:** Start with **MAX98357A** or **NS4168**. These are
"wire and go" — no I2C register configuration, no clock source wiring,
no software initialization beyond I2S. You connect 3 wires (BCLK, LRCLK,
DIN) plus power and ground, and they just work.

Codec chips (ES8311, WM8960) are more capable (built-in mic input, hardware
volume control, EQ) but require an I2C initialization sequence of 10–30
register writes before any audio flows. Add this complexity only if you
need bidirectional audio (mic + speaker) on a single chip.

**Impact on driver code:**

```
                  MAX98357 / NS4168              ES8311 / WM8960
                  ─────────────────              ───────────────
  Init:           Configure I2S only             Configure I2C bus
                  (3 GPIO pins)                  Write ~20 registers via I2C
                                                 THEN configure I2S

  Volume:         Scale PCM samples              Write I2C volume register
                  in software (multiply           (hardware volume control)
                  each sample by 0.0–1.0)

  Shutdown:       Disable I2S channel            Write I2C power-down register
                                                 Then disable I2S

  Lines of code:  ~50                            ~200
```
Design decision: Write this software for the I2S only chips. Delineate with comments what parts control init, volume, and shutdown

### Decision 2: Audio Format (Sample Rate, Bit Depth, Channels)

The audio format determines the I2S configuration and the bandwidth
required on the network link.

| Parameter | Option A (Speech) | Option B (Music) |
|-----------|-------------------|-------------------|
| Sample rate | 16,000 Hz | 44,100 Hz |
| Bit depth | 16-bit | 16-bit |
| Channels | Mono (1 ch) | Stereo (2 ch) |
| Bandwidth | 32 KB/s | 176 KB/s |
| Quality | Good for voice/TTS | CD quality |
| Use case | LLM voice output | Music playback |

**Recommendation:** Use **16 kHz / 16-bit / mono** for LLM/TTS audio.
Voice doesn't benefit from higher sample rates, and 32 KB/s is gentle
on both the cellular/WiFi link and the ring buffer. Most TTS APIs
(OpenAI, ElevenLabs, Google) natively support 16 kHz or 24 kHz output.

Your backend should resample to your chosen format before streaming to
the ESP32. This way the ESP32 never needs to do sample rate conversion.

**Bandwidth calculation:**

```
  Bytes/sec = sample_rate × bit_depth/8 × channels

  16 kHz mono:    16000 × 2 × 1 = 32,000 B/s  = 32 KB/s  = 256 kbps
  24 kHz mono:    24000 × 2 × 1 = 48,000 B/s  = 48 KB/s  = 384 kbps
  44.1 kHz stereo: 44100 × 2 × 2 = 176,400 B/s = 176 KB/s = 1.4 Mbps
```
Design decision: This should be configurable (can be set by just changing the value of a param/config, speech and music mode), this doesn't have to change during runtime though

### Decision 3: Network Transport

How audio bytes travel from your backend to the ESP32.

| Transport | Latency | Complexity | Best For |
|-----------|---------|------------|----------|
| **HTTP chunked** | Medium (~200-500ms buffer) | Low | One-shot TTS responses |
| **WebSocket** | Low (~50-100ms) | Medium | Real-time conversation |
| **HTTP/2 SSE** | Medium | Medium | Streaming with other event data |
| **Raw TCP socket** | Lowest | Higher | Maximum control |

**Recommendation:** Start with **HTTP chunked transfer encoding**. Your
existing `network_app` infrastructure already uses `esp_http_client`.
The backend sets `Transfer-Encoding: chunked` and streams PCM bytes as
they're generated. The ESP32 reads chunks in a loop and pushes them into
the ring buffer.

Move to **WebSocket** if you later need bidirectional audio (mic input
streamed up, speaker output streamed down) or sub-100ms latency. ESP-IDF
provides `esp_websocket_client` for this.

```
  HTTP Chunked (simpler):
  ┌────────┐    GET /audio?text=hello     ┌────────┐
  │ ESP32  │ ──────────────────────────►  │Backend │
  │        │                              │        │
  │        │  ◄── HTTP 200               │        │
  │        │      Content-Type: audio/pcm │        │
  │        │      Transfer-Encoding:      │        │
  │        │        chunked               │        │
  │        │                              │        │
  │        │  ◄── chunk: 1024 bytes PCM  │        │
  │        │  ◄── chunk: 1024 bytes PCM  │        │ ← backend transcodes
  │        │  ◄── chunk: 1024 bytes PCM  │        │   in real time
  │        │  ◄── ...                    │        │
  │        │  ◄── chunk: 0 (end)         │        │
  └────────┘                              └────────┘

  WebSocket (lower latency, bidirectional):
  ┌────────┐    WS upgrade /audio         ┌────────┐
  │ ESP32  │ ──────────────────────────►  │Backend │
  │        │  ◄── WS accept              │        │
  │        │                              │        │
  │        │  ──► text: "Hello world"    │        │ ← ESP sends text prompt
  │        │  ◄── binary: PCM chunk      │        │ ← backend sends audio
  │        │  ◄── binary: PCM chunk      │        │
  │        │  ◄── binary: PCM chunk      │        │
  │        │  ◄── text: "END"            │        │ ← signals completion
  └────────┘                              └────────┘
```

### Decision 4: Ring Buffer Size

The ring buffer sits between the network task and the I2S playback task.
It absorbs network jitter — if the network stalls for 200ms, the buffer
has enough data to keep the speaker playing without gaps.

| Buffer Size | Holds (at 16kHz mono) | Pros | Cons |
|-------------|----------------------|------|------|
| 4 KB | 125 ms | Minimal RAM | Audible gaps on any network hiccup |
| 8 KB | 250 ms | Good balance | Minor gaps possible on slow links |
| **16 KB** | 500 ms | Smooth playback | Uses 16 KB of heap |
| 32 KB | 1000 ms | Very smooth | Adds startup latency (1s fill) |

**Recommendation:** **16 KB** ring buffer. At 32 KB/s throughput, this
holds 500ms of audio — enough to absorb WiFi retransmissions and minor
network stalls without audible gaps. The ESP32-S3 has 512 KB of SRAM,
so 16 KB is ~3% of available memory.

**Pre-buffer strategy:** Before starting I2S playback, wait until the
ring buffer is at least 25–50% full. This prevents the first few hundred
milliseconds from being choppy while the network stream ramps up.

```
  ┌──────────────────────────────────────────────────────────┐
  │                    Ring Buffer (16 KB)                    │
  │                                                          │
  │  Network                                                 │
  │  writes ──►  ┌──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┐      │
  │              │██│██│██│██│██│██│██│  │  │  │  │  │      │
  │              └──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┘      │
  │                    ▲                 ▲                    │
  │                    │  ◄── filled ──► │                    │
  │                    │                 │                    │
  │                 read_ptr          write_ptr               │
  │                    │                                      │
  │                    ▼                                      │
  │              I2S reads ──► speaker                        │
  │                                                          │
  │  Watermarks:                                             │
  │    LOW  (25%) — pause I2S, wait for more data (underrun) │
  │    HIGH (75%) — back-pressure network reads              │
  └──────────────────────────────────────────────────────────┘
```

Design decision: In the final design, use HTTP to implement drivers that can send audio from mic (but worry about the mic implemntation later, add a TODO comment saying this is for the mic part) to the backend and receive from the backend as well (send audio --> backend parse --> receive audio) to speaker. For comments, indicate the step by step process of this within the code, as well as what it is doing at each steo conceptually

### Decision 5: I2S Pin Assignment

The XIAO ESP32-S3 has limited exposed GPIO pins. You need 3 pins for I2S
(or 4–5 if adding I2C for a codec chip). Choose pins that don't conflict
with your existing UART wiring.

**Pins already in use (modem simulator):**

| Pin | XIAO Label | Function |
|-----|-----------|----------|
| GPIO1 | D0 | Driver UART1 TX |
| GPIO2 | D1 | Driver UART1 RX |
| GPIO3 | D2 | Driver UART1 RTS |
| GPIO4 | D3 | Driver UART1 CTS |
| GPIO5 | D4 | Modem UART2 RTS |
| GPIO6 | D5 | Modem UART2 TX |
| GPIO7 | D8 | Modem UART2 RX |
| GPIO8 | D9 | Modem UART2 CTS |

**Available pins on XIAO ESP32-S3:**

| Pin | XIAO Label | Suggested I2S Function |
|-----|-----------|----------------------|
| GPIO9 | D10 (SDA) | I2S BCLK (bit clock) |
| GPIO10 | D7 (SCL) | I2S WS / LRCLK (word select) |
| GPIO44 | D6 | I2S DOUT (data out to amp) |

If using WiFi mode (modem pins are free), you have more flexibility.
In WiFi mode, D0–D5 are all available and you can pick any three.

**If your amp also needs I2C** (codec chips like ES8311): use D10/D7
for I2C (SDA/SCL) since they're the default I2C pins, and reassign I2S
to D0–D2 (which are free in WiFi mode).

Design decision: This should just be configurable, have not decided yet 

### Decision 6: Volume Control Strategy

| Strategy | How | Pros | Cons |
|----------|-----|------|------|
| **Software scaling** | Multiply each PCM sample by a float [0.0, 1.0] before writing to I2S | No extra hardware | Uses CPU (~1% at 16kHz) |
| **Hardware gain pin** | MAX98357 GAIN pin: tie to GND/VCC/float for 3dB/6dB/9dB/12dB/15dB | Zero CPU cost | Only 5 fixed levels |
| **I2C volume register** | ES8311/WM8960 have 8-bit volume registers | Fine-grained, zero CPU | Requires I2C bus + codec chip |
| **External potentiometer** | Analog pot on amp output | User-adjustable | Extra component, no software control |

**Recommendation:** Use **software scaling** for now. It's universal
(works with any amp), requires zero extra hardware, and the CPU cost of
multiplying 16,000 samples/sec by a float is negligible.

```c
void apply_volume(int16_t *samples, size_t count, float volume) {
    for (size_t i = 0; i < count; i++) {
        samples[i] = (int16_t)(samples[i] * volume);
    }
}
```
Design decision: Implement software scaling with the config option to turn it off. Use comments to indicate volume scaling code

IMPORTANT OVERALL DESIGN DECISIONS: Each task implemented here should correspond to its own file (refer to the other drivers if needed). Each task should only have one init call in app main and a task call if possible, dont put extraneous stuff in the app main. THE GOAL IS TO KEEP DESIGN MODULAR AND EASY TO DEBUG/TEST

---

## Driver Architecture

### File Structure

Following the existing project conventions, the speaker driver adds two
files to `main/`:

```
  main/
  ├── main.c               (modified — add speaker init + task creation)
  ├── speaker_driver.c      (new — I2S + ring buffer + playback logic)
  ├── speaker_driver.h      (new — public API + config struct)
  ├── modem_driver.c         (existing)
  ├── modem_driver.h         (existing)
  ├── wifi_driver.c          (existing)
  ├── network_app.c          (existing — may add audio HTTP endpoint)
  └── ...
```

Update `main/CMakeLists.txt` to include `speaker_driver.c` in the source list.

### Config Struct (matching existing pattern)

```c
typedef enum {
    SPEAKER_STATE_IDLE,         // initialized, not playing
    SPEAKER_STATE_BUFFERING,    // receiving data, filling pre-buffer
    SPEAKER_STATE_PLAYING,      // actively writing to I2S
    SPEAKER_STATE_DRAINING,     // network done, playing remaining buffer
    SPEAKER_STATE_STOPPED,      // playback complete
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
```

This mirrors the `modem_driver_config_t` pattern: hardware pins, operating
parameters, and embedded state.

### Public API

```c
/* Initialize I2S peripheral and allocate ring buffer. */
void speaker_init(void);

/* Start streaming: connect to URL, receive PCM, play through speaker. */
void speaker_play_url(const char *url);

/* Stop playback immediately (discards buffer). */
void speaker_stop(void);

/* Set volume. Takes effect on next buffer read. */
void speaker_set_volume(float volume);

/* Query current state. */
speaker_state_t speaker_get_state(void);

/* FreeRTOS task entry point for I2S playback (internal). */
void speaker_playback_task(void *param);
```

### State Machine

```
                          speaker_init()
                               │
                               ▼
                        ┌─────────────┐
                        │    IDLE      │◄──────────────────────────┐
                        └──────┬──────┘                            │
                               │ speaker_play_url() called         │
                               │ — launches network fetch          │
                               ▼                                   │
                        ┌─────────────┐                            │
                        │  BUFFERING  │                            │
                        │             │                            │
                        │ Filling ring│                            │
                        │ buffer from │                            │
                        │ network     │                            │
                        └──────┬──────┘                            │
                               │ ring buffer ≥ prebuffer_threshold │
                               ▼                                   │
                        ┌─────────────┐                            │
                        │  PLAYING    │                            │
                        │             │                            │
                        │ I2S writes  │                            │
                        │ from ring   │                            │
                        │ buffer      │                            │
                        │             │                            │
                        │ Network     │                            │
                        │ continues   │                            │
                        │ filling     │                            │
                        └──────┬──────┘                            │
                               │ network signals end-of-stream     │
                               ▼                                   │
                        ┌─────────────┐                            │
                        │  DRAINING   │                            │
                        │             │                            │
                        │ No more     │                            │
                        │ network RX  │                            │
                        │ Playing     │                            │
                        │ remaining   │                            │
                        │ buffer      │                            │
                        └──────┬──────┘                            │
                               │ ring buffer empty                 │
                               ▼                                   │
                        ┌─────────────┐                            │
                        │  STOPPED    │────────────────────────────┘
                        └─────────────┘   (auto-return or explicit)


  At any point:
    speaker_stop() ──────────► IDLE (immediate, discards buffer)
    fatal I2S error ─────────► ERROR
```

---

## Task Architecture

Two FreeRTOS tasks run in parallel, connected by a shared ring buffer.
This is the standard producer–consumer pattern.

```
  ┌─────────────────────────┐         ┌─────────────────────────────┐
  │   NETWORK TASK          │         │   PLAYBACK TASK              │
  │   (producer)            │         │   (consumer)                 │
  │                         │         │                              │
  │   1. Open HTTP conn     │         │   1. Wait for BUFFERING      │
  │   2. Read chunk from    │  ring   │      → PLAYING transition    │
  │      response body      │ buffer  │   2. Read N bytes from       │
  │   3. Write bytes to ────┼────────►│      ring buffer             │
  │      ring buffer        │         │   3. Apply volume scaling    │
  │   4. If buffer full,    │         │   4. Write to I2S channel    │
  │      block until space  │         │   5. If underrun (buffer     │
  │   5. Loop until HTTP    │         │      empty + still playing), │
  │      transfer complete  │         │      write silence           │
  │   6. Signal end-of-     │         │   6. Loop until STOPPED      │
  │      stream             │         │                              │
  └─────────────────────────┘         └─────────────────────────────┘
          │                                        │
          │  Priority: TASK_PRIORITY               │  Priority: TASK_PRIORITY + 1
          │  Core: any (or DRIVER_CORE)            │  Core: any (or DRIVER_CORE)
          │  Stack: 4096 bytes                     │  Stack: 4096 bytes
          │                                        │
          │  Blocks on: ring buffer full            │  Blocks on: ring buffer empty
          │             esp_http_client_read()      │             i2s_channel_write()
          └────────────────────────────────────────┘
```

**Why two tasks?** Network reads and I2S writes have fundamentally
different timing. The network delivers data in irregular bursts (big chunk,
then nothing for 100ms, then another big chunk). I2S consumes data at a
rock-steady 32 KB/s. The ring buffer and two-task design decouple these
two timing domains.

**Why the playback task has higher priority:** If both tasks are runnable,
the playback task should run first to prevent I2S underruns. A network
read can wait 10ms without consequence; an I2S underrun produces an
audible click/pop.

### Ring Buffer Implementation

ESP-IDF provides `xRingbuffer` (FreeRTOS ring buffer), but for fixed-size
PCM chunks, a simple byte-oriented ring buffer using a FreeRTOS stream
buffer is cleaner:

```c
#include "freertos/stream_buffer.h"

StreamBufferHandle_t audio_stream;

// Created once in speaker_init()
audio_stream = xStreamBufferCreate(RING_BUFFER_SIZE, TRIGGER_LEVEL);

// Network task (producer)
xStreamBufferSend(audio_stream, pcm_chunk, chunk_len, pdMS_TO_TICKS(100));

// Playback task (consumer)
size_t received = xStreamBufferReceive(audio_stream, play_buf, play_buf_size,
                                        pdMS_TO_TICKS(50));
```

`xStreamBufferSend` blocks if the buffer is full (back-pressures the network).
`xStreamBufferReceive` blocks if the buffer is empty (pauses playback).
The `TRIGGER_LEVEL` parameter sets the minimum number of bytes that must be
in the buffer before `xStreamBufferReceive` unblocks — useful for ensuring
we always write complete sample frames.

---

## Backend Interface Design

### Request Flow

```
  ESP32                              Your Backend Server
  ─────                              ──────────────────

  1. User triggers audio
     (button press, command, etc.)
           │
           ├──► POST /api/speak
           │    Body: { "text": "Hello, how are you?" }
           │    Headers:
           │      Content-Type: application/json
           │      X-Audio-Format: pcm;rate=16000;bits=16;channels=1
           │
           │                         2. Backend receives text
           │                            ├── Calls LLM API (if needed)
           │                            │   e.g. OpenAI Chat Completions
           │                            │   → gets response text
           │                            │
           │                            ├── Calls TTS API
           │                            │   e.g. OpenAI TTS, ElevenLabs
           │                            │   → gets audio (MP3/Opus/PCM)
           │                            │
           │                            ├── Transcodes to raw PCM
           │                            │   ffmpeg -f s16le -ar 16000 -ac 1
           │                            │
           │                            ▼
           │    ◄── HTTP 200 OK
           │        Content-Type: application/octet-stream
           │        Transfer-Encoding: chunked
           │        X-Audio-Format: pcm;rate=16000;bits=16;channels=1
           │
           │    ◄── chunk: [1024 bytes raw PCM]
           │    ◄── chunk: [1024 bytes raw PCM]
           │    ◄── chunk: [512 bytes raw PCM]
           │    ◄── chunk: 0 (end of stream)
           │
  3. ESP32 plays each chunk as it arrives
```

### Why Offload Decoding to the Backend

| Concern | Decode on ESP32 | Decode on Backend |
|---------|----------------|-------------------|
| **RAM** | +30–50 KB for decoder lib | No extra RAM needed |
| **CPU** | MP3 decode: ~15% of one core | ESP32 CPU fully free |
| **Flash** | +50–100 KB firmware size | No firmware impact |
| **Format flexibility** | Locked to one codec | Handle any TTS API output format |
| **Update path** | Requires OTA to change codec | Change backend code, ESP untouched |
| **Latency** | Decode adds ~10ms per chunk | Decode on server is ~instant |

The backend is trivially capable of transcoding. A Python backend using
ffmpeg can convert any audio format to raw PCM in a single subprocess call.
The ESP32 never needs to know what format the TTS API originally returned.

### Audio Format Contract

The ESP32 and backend must agree on the exact PCM format. Define this once
and enforce it on both sides:

```
  Format:     Raw PCM (no headers, no container)
  Encoding:   Signed 16-bit little-endian (s16le)
  Sample rate: 16,000 Hz
  Channels:   1 (mono)
  Byte order: Little-endian (native to ESP32)
```

The backend MUST send audio in exactly this format. The ESP32 does no
format validation — it writes whatever bytes it receives directly to I2S.
A format mismatch produces noise, not an error.

---

## I2S Configuration Detail

### ESP-IDF I2S Driver (v5.x new driver API)

ESP-IDF v5.x introduced a new I2S driver (`driver/i2s_std.h`) replacing
the legacy `driver/i2s.h`. The new API uses channel handles and is the
recommended path forward.

```
  I2S initialization sequence:
  ┌─────────────────────────────────────────────────────────────────┐
  │                                                                 │
  │   1. i2s_new_channel()      ──► Creates a TX channel handle     │
  │                                  (no RX — we're output only)    │
  │                                                                 │
  │   2. i2s_channel_init_      ──► Configures:                     │
  │      std_mode()                  • Clock: sample rate, MCLK     │
  │                                  • Slot: bit depth, channels,   │
  │                                          Philips/MSB/PCM mode   │
  │                                  • GPIO: BCLK, WS, DOUT pins    │
  │                                                                 │
  │   3. i2s_channel_enable()   ──► Starts the I2S peripheral       │
  │                                  Clock signals begin toggling    │
  │                                                                 │
  │   4. i2s_channel_write()    ──► Sends PCM data (blocking)       │
  │      (called in a loop)          Returns bytes_written           │
  │                                                                 │
  │   5. i2s_channel_disable()  ──► Stops the I2S peripheral        │
  │      (on stop/cleanup)          Clock signals stop               │
  │                                                                 │
  │   6. i2s_del_channel()      ──► Frees the channel handle        │
  │                                                                 │
  └─────────────────────────────────────────────────────────────────┘
```

### I2S Signal Wiring

```
  ESP32-S3 XIAO                           MAX98357A Breakout
  ┌──────────────┐                         ┌──────────────┐
  │              │                         │              │
  │  BCLK (D10) ├────────── BCLK ────────►│ BCLK         │
  │              │                         │              │
  │  WS   (D7)  ├────────── LRCLK ───────►│ LRC          │
  │              │                         │              │
  │  DOUT (D6)  ├────────── DIN ──────────►│ DIN          │
  │              │                         │              │
  │  3V3        ├────────── VIN ──────────►│ VIN          │
  │              │                         │              │
  │  GND        ├────────── GND ──────────►│ GND          │
  │              │                         │              │
  └──────────────┘                         │       OUT+ ──┼──► Speaker +
                                           │       OUT- ──┼──► Speaker -
                                           │              │
                                           │  GAIN ───────┼──► (see table)
                                           │  SD   ───────┼──► 3V3 (enable)
                                           └──────────────┘

  MAX98357 GAIN pin configuration:
  ┌──────────────┬──────────┐
  │ GAIN wiring  │ Gain     │
  ├──────────────┼──────────┤
  │ Float (NC)   │ +9 dB    │
  │ GND          │ +12 dB   │
  │ VIN          │ +15 dB   │
  │ 100kΩ to GND │ +6 dB    │
  │ 100kΩ to VIN │ +3 dB    │
  └──────────────┴──────────┘

  SD (shutdown) pin: HIGH = enabled, LOW = shutdown (mute + low power)
```

---

## Implementation Plan — Step by Step

Work through these in order. Each step produces a testable checkpoint.
Don't move to the next step until the current one works.

### Step 1: I2S Smoke Test — Play a Tone

**Goal:** Verify I2S wiring and amplifier by playing a generated sine wave.
No network, no ring buffer, no tasks — just raw I2S output.

| What to implement | How to verify |
|-------------------|---------------|
| `speaker_init()` — configure I2S channel | No crash, I2S clocks visible on logic analyzer or oscilloscope |
| Generate a 440 Hz sine wave in a buffer | Audible tone from speaker |
| Write buffer to I2S in a loop | Continuous, clean tone (no clicks/pops) |

```
  Implementation outline:

  1. Create i2s_chan_handle_t (TX only)
  2. Configure: 16kHz, 16-bit, mono, Philips mode
  3. Set GPIO pins: BCLK, WS, DOUT
  4. Enable channel
  5. Generate 1 period of 440Hz sine into a buffer:
       samples_per_period = 16000 / 440 ≈ 36 samples
       for i in 0..36: buf[i] = (int16_t)(32767 * sin(2π * i / 36))
  6. In a loop: i2s_channel_write(handle, buf, sizeof(buf), ...)

  Expected result: A clear 440 Hz tone plays continuously.
  If you hear noise: check pin assignments and I2S mode (Philips vs PCM).
  If silence: check SD (shutdown) pin, power wiring, and VIN voltage.
```

**This is the most important step.** If you can play a sine wave, you know:
- I2S peripheral is configured correctly
- Wiring to the amp is correct
- The amp and speaker are working
- Pin assignments don't conflict with other peripherals

### Step 2: Software Volume Control

**Goal:** Add volume scaling so you can verify it works with the sine wave.

| What to implement | How to verify |
|-------------------|---------------|
| `speaker_set_volume(float vol)` | Call with 0.5, tone is quieter |
| `apply_volume()` — scale PCM samples | Call with 0.0, silence |

### Step 3: Ring Buffer + Playback Task

**Goal:** Decouple audio generation from playback. This introduces the
two-task architecture without network complexity.

| What to implement | How to verify |
|-------------------|---------------|
| Create `StreamBufferHandle_t` in `speaker_init()` | No crash |
| `speaker_playback_task()` — reads from stream buffer, writes to I2S | Tone still plays |
| Producer: a test task that writes sine wave into stream buffer | Identical sound to Step 1 |

```
  Test setup:

  ┌─────────────────┐     stream      ┌─────────────────┐
  │  Test Producer   │    buffer      │  Playback Task   │
  │  (sine wave gen) │ ──────────►   │  (I2S write)     │
  └─────────────────┘                 └─────────────────┘

  If the sine wave sounds identical to Step 1, the ring buffer and
  task architecture are working correctly. If you hear clicks or gaps,
  the ring buffer is too small or the task priorities are wrong.
```

### Step 4: Network Streaming — HTTP Chunked

**Goal:** Replace the test sine wave producer with a real network client
that fetches PCM audio from your backend.

| What to implement | How to verify |
|-------------------|---------------|
| `speaker_play_url(url)` — creates HTTP client, streams response into ring buffer | Audio plays from backend |
| Pre-buffer logic: wait for N bytes before starting I2S | Smooth start, no initial stutter |
| End-of-stream detection: HTTP transfer complete → DRAINING state | Audio plays to completion, then stops |

```
  Test setup:

  1. Set up a simple backend (Python Flask/FastAPI) that returns
     a WAV file converted to raw PCM:

       @app.get("/test-audio")
       def test_audio():
           pcm = convert_wav_to_pcm("test.wav")
           return Response(pcm, media_type="application/octet-stream")

  2. On ESP32, after WiFi connects:

       speaker_play_url("http://your-server:5000/test-audio");

  3. Expected: The audio file plays through the speaker clearly.
```

### Step 5: LLM/TTS Integration

**Goal:** Full pipeline — send text to your backend, backend calls
LLM + TTS, ESP32 plays the streamed response.

| What to implement | How to verify |
|-------------------|---------------|
| Modify backend to accept text, call TTS API, stream PCM | POST with text, hear spoken response |
| Add JSON request body support to `speaker_play_url()` | Backend receives the text correctly |
| Handle TLS (HTTPS) if your backend requires it | No TLS handshake errors |

### Step 6: Error Handling & Edge Cases

| Scenario | Behavior |
|----------|----------|
| Network disconnects mid-stream | Play remaining buffer, then STOPPED. Log warning. |
| Backend returns non-200 HTTP status | Transition to ERROR. Log the status code. |
| Ring buffer underrun during playback | Write silence (zeros) to I2S. Log "underrun" once per event. |
| `speaker_stop()` called during playback | Flush ring buffer. Disable I2S. Return to IDLE immediately. |
| `speaker_play_url()` called while already playing | Stop current playback, start new stream. |
| Backend sends wrong PCM format | Garbage audio. No detection possible without header metadata. |
| I2S write timeout | Retry once. If persistent, transition to ERROR. |

### Step 7 (Optional): Enhancements

These are stretch goals. Implement only if needed.

| Enhancement | Complexity | Value |
|-------------|-----------|-------|
| WebSocket transport | Medium | Lower latency for real-time conversation |
| Bidirectional audio (mic input) | Medium | Voice-in, voice-out with LLM |
| Codec chip (ES8311) support | Medium | Hardware volume, mic preamp |
| Audio status callbacks | Low | Notify app layer of state changes |
| Multi-format support (WAV header parsing) | Low | Play standard WAV files directly |
| DMA buffer tuning | Low | Reduce I2S latency further |

---

## Integration with Existing Codebase

### main.c Changes

The speaker driver integrates the same way as the modem driver — init
function plus a pinned task:

```
  void app_main(void)
  {
      // ... existing init ...
      esp_netif_init();
      esp_event_loop_create_default();
      network_app_init();

      // Connectivity (pick one)
      wifi_driver_init();
      // OR modem init sequence...

      // Audio output
      speaker_init();                    // ← new
      xTaskCreate(                       // ← new
          speaker_playback_task,
          "speaker",
          TASK_STACK_SIZE,
          NULL,
          TASK_PRIORITY + 1,    // higher than network tasks
          NULL
      );

      // ... existing task creation ...
  }
```

### Network App Integration

The speaker can be triggered from `network_app.c` or any other module
that has network access:

```c
// After WiFi/PPP is connected and IP is acquired:
network_app_wait_for_connection();
speaker_play_url("https://your-backend.com/api/speak?text=hello");
```

Or from a dedicated "voice assistant" task that manages the
conversation loop:

```c
void voice_assistant_task(void *param) {
    network_app_wait_for_connection();
    while (1) {
        // 1. Wait for trigger (button press, wake word, etc.)
        // 2. Record mic audio (future)
        // 3. Send to backend
        // 4. Play response
        speaker_play_url("https://backend.example.com/api/chat");
        // 5. Wait for playback to finish
        while (speaker_get_state() != SPEAKER_STATE_STOPPED) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
```

### sdkconfig.defaults Additions

No special sdkconfig options are needed for basic I2S output. The ESP-IDF
I2S driver is enabled by default. If you add HTTPS support for the audio
endpoint, you may need:

```
# TLS for HTTPS audio streaming (if not already enabled)
CONFIG_ESP_TLS_USING_MBEDTLS=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
```

---

## Memory Budget

| Component | RAM Usage | Notes |
|-----------|----------|-------|
| Ring buffer | 16,384 bytes | Configurable via `ring_buffer_size` |
| I2S DMA buffers | ~4,096 bytes | Managed internally by I2S driver (8 × 512B default) |
| Playback task stack | 4,096 bytes | Standard task stack size |
| Network task stack | 4,096 bytes | Reuses existing network task or dedicated |
| I2S channel handle | ~200 bytes | Internal ESP-IDF allocation |
| **Total** | **~29 KB** | ~5.6% of ESP32-S3's 512KB SRAM |

This is conservative. If RAM is tight, the ring buffer can be reduced
to 8 KB (250ms at 16kHz mono) with minimal impact on audio quality over
a stable WiFi connection.

---

## Troubleshooting Guide

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| No sound at all | SD pin floating or LOW | Tie MAX98357 SD pin to 3V3 |
| No sound at all | Wrong DOUT pin | Verify GPIO number matches physical wiring |
| No sound at all | I2S not enabled | Ensure `i2s_channel_enable()` was called |
| Loud noise / static | PCM format mismatch (sample rate, endianness) | Verify backend sends s16le at expected sample rate |
| Clicks / pops between chunks | Ring buffer underrun | Increase ring buffer size or pre-buffer threshold |
| Audio plays too fast / chipmunk | Sample rate mismatch (e.g. 24kHz data at 16kHz I2S) | Ensure backend resamples to match I2S config |
| Audio plays too slow / deep | Sample rate mismatch (e.g. 8kHz data at 16kHz I2S) | Same — match sample rates |
| Audio works but very quiet | Volume scaling too low, or GAIN pin wrong | Check `volume` float value; check GAIN wiring |
| Audio works but distorted | Clipping — PCM samples exceed int16 range after volume scale | Ensure volume ≤ 1.0; check amp power supply |
| Heap allocation fails | Not enough free RAM | Reduce ring buffer size; check for memory leaks |
| I2S write blocks forever | DMA buffer full, no consumer | Check that I2S channel is enabled and clocks are running |

---

## Appendix A: Wokwi Simulation

The current `diagram.json` only has UART loopback wires. To simulate the
speaker in Wokwi, you would need to add a virtual I2S device. Wokwi does
not currently have a native I2S speaker component, so speaker testing
requires real hardware or a custom Wokwi chip.

For development without hardware, the playback task can write to a log
instead of I2S:

```c
#ifdef SPEAKER_SIMULATE
    ESP_LOGI(TAG, "I2S write: %d bytes (simulated)", bytes_to_write);
#else
    i2s_channel_write(tx_handle, buf, bytes_to_write, &written, timeout);
#endif
```

This lets you develop and test the full pipeline (network → buffer → task)
in simulation, then switch to real I2S output on hardware.

---

## Appendix B: Comparison with Modem Driver Architecture

| Aspect | Modem Driver | Speaker Driver |
|--------|-------------|----------------|
| Peripheral | UART | I2S |
| Config struct | `modem_driver_config_t` | `speaker_config_t` |
| State machine | 7 states (AT → PPP → IP) | 5 states (IDLE → PLAYING → STOPPED) |
| Number of tasks | 1 (modem_driver_task) | 2 (network producer + I2S consumer) |
| Inter-task buffer | UART hardware FIFO | Software ring buffer (StreamBuffer) |
| External dependency | sim_modem (UART peer) | Backend server (HTTP peer) |
| Data direction | Bidirectional (TX+RX) | Receive only (network → speaker) |
| Protocol complexity | AT commands + PPP + IP | Raw PCM bytes — no protocol |
| Init complexity | UART config + AT handshake | I2S config (3 pins, done) |
