# Mic Driver — Workflow

This document walks through the mic driver's complete lifecycle step by
step. Each section explains what happens conceptually, which code runs, and
how data moves between tasks, buffers, and hardware. The mic driver mirrors
the speaker driver — same architecture, reversed direction.

---

## High-Level Architecture

Two FreeRTOS tasks cooperate in a producer–consumer pattern, connected by a
shared stream buffer. The capture task is permanent (created once at boot);
the upload task is ephemeral (created per upload request, self-deletes).

This is the mirror image of the speaker driver: the speaker's network task
is the producer and its playback task is the consumer. For the mic, the
capture task is the producer and the upload task is the consumer.

```
  ┌──────────────────────────────────────────────────────────────────────────┐
  │                          ESP32-S3 XIAO                                   │
  │                                                                          │
  │   ┌─────────────────────┐                 ┌───────────────────────────┐  │
  │   │  mic_capture_task() │                 │  mic_upload()             │  │
  │   │  (permanent,        │                 │  (caller — voice_asst)    │  │
  │   │   priority 6)       │                 │                           │  │
  │   │                     │                 └────────┬──────────────────┘  │
  │   │  Phase 1: Sleep     │                          │ spawns              │
  │   │  Phase 2: Enable RX │                          ▼                     │
  │   │  Phase 3: Read I2S  │   stream buffer ┌────────────────────┐         │
  │   │  Phase 4: Disable   │  ┌────────────┐ │ mic_upload_task    │         │
  │   │                     │  │ 16 KB ring │ │ (ephemeral,        │         │
  │   │  I2S read ──────────├──►  buffer    ├─► priority 5)        │         │
  │   │                     │  │            │ │                    │         │
  │   │  gain applied       │  └────────────┘ │ buffer → HTTP POST │         │
  │   │  before buffer      │                 │ → backend          │         │
  │   └──────────┬──────────┘                 └────────────────────┘         │
  │              │                                                           │
  └──────────────┼────────────────────────────────────────────────────────── ┘
                 │
                 │  I2S bus (3 wires — separate I2S1)
  BCLK (GPIO1)  ─┤
  WS   (GPIO2)  ─┤
  DIN  (GPIO3)  ─┘
                 │
  ┌──────────────▼── ┐
  │  MEMS Microphone │
  │  (INMP441)       │
  │                  │
  │  Sound → Digital │
  │  I2S standard    │
  └──────────────────┘
```

**Comparison with speaker driver architecture:**

```
  Speaker (output):                      Mic (input):
  ──────────────────                     ────────────────

  Network task (producer)                Capture task (producer)
       │                                      │
       │ HTTP response chunks                 │ I2S RX reads
       ▼                                      ▼
  ┌────────────┐                         ┌────────────┐
  │  Stream    │                         │  Stream    │
  │  Buffer    │                         │  Buffer    │
  └─────┬──────┘                         └─────┬──────┘
        │                                      │
        ▼                                      ▼
  Playback task (consumer)               Upload task (consumer)
       │                                      │
       │ volume → i2s_channel_write()         │ xStreamBufferReceive()
       ▼                                      │ → esp_http_client_write()
  I2S TX → amp → speaker                      ▼
                                         HTTP POST → backend
```

---

## State Machine

Every state transition is driven by exactly one actor (noted in parentheses).
The state variable lives in `s_mic_cfg.state` and is read by both tasks.

```
                        mic_init()
                             │
                             ▼
                      ┌─────────────┐
                ┌────►│    IDLE     │ ◄──────────────────────────┐
                │     └──────┬──────┘                            │
                │            │ mic_start()                       │
                │            │   (voice_assistant — button press)│
                │            ▼                                   │
                │     ┌─────────────┐                            │
                │     │  RECORDING  │                            │
                │     │             │                            │
                │     │ capture task│                            │
                │     │ reads I2S,  │                            │
                │     │ applies     │                            │
                │     │ gain,       │                            │
                │     │ fills buffer│                            │
                │     └──────┬──────┘                            │
                │            │ mic_stop()                        │
                │            │   (voice_assistant — button       │
                │            │    release)                       │
                │            ▼                                   │
                │     ┌─────────────┐                            │
                │     │    IDLE     │                            │
                │     │ (data in    │                            │
                │     │  buffer)    │                            │
                │     └──────┬──────┘                            │
                │            │ mic_upload(url)                   │
                │            │   (voice_assistant)               │
                │            ▼                                   │
                │     ┌─────────────┐                            │
                │     │  UPLOADING  │                            │
                │     │             │                            │
                │     │ upload task │                            │
                │     │ reads buffer│                            │
                │     │ HTTP POST   │                            │
                │     │ to backend  │                            │
                │     └──────┬──────┘                            │
                │            │ upload complete                   │
                │            ▼                                   │
                │     ┌─────────────┐                            │
                │     │    DONE     │────────────────────────────┘
                │     └─────────────┘   (upload task → auto IDLE)
                │
                │  mic_start() from any state → resets buffer → RECORDING
                └──────────────────────────────────────────────
           ERROR ◄── I2S read failure, HTTP upload failure, alloc failure
```

**Comparison with speaker state machine:**

```
  Speaker states:           Mic states:
  ──────────────            ───────────
  IDLE                      IDLE
  BUFFERING                 RECORDING       (I2S active, filling buffer)
  PLAYING                   (no equivalent — mic doesn't stream live)
  DRAINING                  UPLOADING       (draining buffer to network)
  STOPPED                   DONE            (transient, auto-returns to IDLE)
  ERROR                     ERROR

  The speaker has 5 active states because it streams in real-time
  (network and I2S run concurrently). The mic has 3 active states
  because recording completes fully before upload begins.
```

---

## Workflow Step by Step

### Step 0: Boot — `app_main()` in `main.c`

When the ESP32 boots, `app_main()` runs one-time setup. The mic driver
follows the same init pattern as the speaker: one init call, one task
creation. The mic init runs AFTER the speaker init — this ordering is
required for the full-duplex I2S0 mode (where the speaker creates both
TX+RX handles and the mic retrieves the RX handle).

```
  app_main()
  │
  ├── esp_netif_init()                  TCP/IP stack
  ├── esp_event_loop_create_default()   event system
  ├── network_app_init()                IP/SNTP event handlers
  ├── wifi_driver_init()                Wi-Fi connect
  │
  ├── speaker_init()                    speaker I2S TX + buffer
  ├── xTaskCreatePinnedToCore(
  │       speaker_playback_task, ...)   priority 6, core 1
  │
  ├── mic_init()  ◄──────────────────── one init call (AFTER speaker)
  ├── xTaskCreatePinnedToCore(          one task creation
  │       mic_capture_task,
  │       "mic_cap",
  │       4096,                         stack size
  │       NULL,
  │       TASK_PRIORITY + 1,            priority 6 (higher than net tasks)
  │       NULL,
  │       DRIVER_CORE)                  pinned to core 1
  │
  ├── voice_assistant_init()            push-to-talk button GPIO setup
  ├── xTaskCreatePinnedToCore(
  │       voice_assistant_task, ...)    priority 5, core 1
  │
  ├── xTaskCreatePinnedToCore(network_app_task, ...)
  └── xTaskCreatePinnedToCore(test_network_task, ...)
```

**What `mic_init()` does internally:**

```
  mic_init()
  │
  │  ┌─────────────────────────────────────────────────────────┐
  │  │  I2S RX INITIALIZATION (skipped if MIC_SIMULATE = 1)    │
  │  │                                                         │
  │  │  Step 1: Acquire I2S RX channel handle                  │
  │  │                                                         │
  │  │  ┌── MIC_I2S_NUM == 1 (default — separate) ──────────┐  │
  │  │  │                                                   │  │
  │  │  │  i2s_new_channel(I2S_NUM_1, NULL, &s_rx_handle)   │  │
  │  │  │    → Creates RX-only channel on I2S1              │  │
  │  │  │    → Completely independent of speaker            │  │
  │  │  │    → ESP32 is MASTER (generates clocks)           │  │
  │  │  │    → NULL for TX handle (mic = input only)        │  │
  │  │  │                                                   │  │
  │  │  └───────────────────────────────────────────────────┘  │
  │  │                                                         │
  │  │  ┌── MIC_I2S_NUM == 0 (optional — full-duplex) ──────┐  │
  │  │  │                                                   │  │
  │  │  │  s_rx_handle = speaker_get_rx_handle()            │  │
  │  │  │    → Speaker already created TX+RX on I2S0        │  │
  │  │  │    → Requires SPEAKER_FULL_DUPLEX=1               │  │
  │  │  │    → Requires speaker_init() called first         │  │
  │  │  │    → If NULL → state = ERROR, return              │  │
  │  │  │                                                   │  │
  │  │  └───────────────────────────────────────────────────┘  │
  │  │                                                         │
  │  │  Step 2: i2s_channel_init_std_mode(s_rx_handle)         │
  │  │    → Philips standard I2S mode (RX)                     │
  │  │    → Clock: sample rate from config (16 kHz default)    │
  │  │    → Slots: 16-bit, mono                                │
  │  │    → GPIO: BCLK, WS, DIN (configurable per I2S mode)    │
  │  │    → MCLK unused, DOUT unused                           │
  │  │                                                         │
  │  │  Step 3: Channel is NOT enabled yet                     │
  │  │    → No clocks toggling, mic is idle                    │
  │  │    → Enabled later by the capture task                  │
  │  └─────────────────────────────────────────────────────────┘
  │
  │  ┌─────────────────────────────────────────────────────────┐
  │  │  STREAM BUFFER ALLOCATION                               │
  │  │                                                         │
  │  │  xStreamBufferCreate(16384, 1)                          │
  │  │    → 16 KB ring buffer (500 ms of audio at 16 kHz)      │
  │  │    → Trigger level = 1 byte (wake on any data)          │
  │  │                                                         │
  │  │  If allocation fails → state = ERROR, return            │
  │  └─────────────────────────────────────────────────────────┘
  │
  └── state = IDLE
```

After `mic_init()` returns, the system is in this state:

```
  I2S RX channel: exists, configured, DISABLED (no clocks)
  Stream buffer: allocated, empty
  Capture task: running, sleeping in Phase 1 (polling for RECORDING)
  Upload task: does not exist
  State: IDLE
```

---

### Step 1: Start Recording — `mic_start()`

The voice assistant calls `mic_start()` when the push-to-talk button is
pressed. This is a synchronous call — it returns after the state is set.
The capture task (already running, polling) sees the new state and begins.

```
  voice_assistant_task (on button press)
  │
  └── mic_start()
      │
      │  1. If already RECORDING → return (no-op)
      │
      │  2. If UPLOADING → kill upload task (previous upload in progress)
      │
      │  3. state = IDLE
      │     vTaskDelay(50ms)
      │     → Brief pause for capture task to exit any previous loop
      │     → Ensures stream buffer is not being written to
      │
      │  4. s_captured_bytes = 0
      │     xStreamBufferReset(s_audio_stream)
      │     → Clear stale data from any previous recording
      │
      │  5. state = RECORDING
      │     → Capture task wakes up on next poll (≤20ms)
      │
      └── Returns
```

After `mic_start()` returns:

```
  I2S RX channel: configured, DISABLED (not yet — capture task enables it)
  Stream buffer: empty, ready for new audio
  Capture task: about to wake from Phase 1 poll, sees RECORDING
  Upload task: does not exist
  State: RECORDING
  Byte counter: 0
```

---

### Step 2: Capture Audio — `mic_capture_task()`

The capture task is the **producer**. It reads raw PCM samples from the I2S
peripheral, applies gain, and writes them into the stream buffer. This task
runs for the lifetime of the application, cycling between sleeping (waiting
for RECORDING) and actively capturing.

```
  mic_capture_task()      (created once at boot, runs forever)
  │
  └── while (1) {                      outer loop — repeats per recording session
      │
      │  ┌──────────────────────────────────────────────────────────────┐
      │  │  PHASE 1: Wait for RECORDING state                           │
      │  │                                                              │
      │  │  while (state != RECORDING)                                  │
      │  │      vTaskDelay(20ms);                                       │
      │  │                                                              │
      │  │  The task polls every 20ms. Low overhead: ~0.05% CPU.        │
      │  │  Wakes when mic_start() sets state = RECORDING.              │
      │  └──────────────────────────────────────────────────────────────┘
      │
      │  ┌──────────────────────────────────────────────────────────────┐
      │  │  PHASE 2: Enable I2S RX                                      │
      │  │                                                              │
      │  │  i2s_channel_enable(s_rx_handle)                             │
      │  │    → BCLK and WS clocks start toggling                       │
      │  │    → Mic sees clock edges, begins shifting out samples       │
      │  │    → DMA starts filling internal I2S buffers                 │
      │  │    → s_i2s_enabled = true                                    │
      │  │                                                              │
      │  │  (skipped in MIC_SIMULATE mode)                              │
      │  └──────────────────────────────────────────────────────────────┘
      │
      │  ┌──────────────────────────────────────────────────────────────┐
      │  │  PHASE 3: Capture loop                                       │
      │  │                                                              │
      │  │  while (state == RECORDING) {                                │
      │  │                                                              │
      │  │      ┌──────────────────────────────────┐                    │
      │  │      │  3a. Read from I2S peripheral    │                    │
      │  │      │                                  │                    │
      │  │      │  bytes_read = i2s_channel_read(  │                    │
      │  │      │      rx_handle, capture_buf,     │                    │
      │  │      │      1024, timeout=1000ms)       │                    │
      │  │      │                                  │                    │
      │  │      │  Blocks until DMA has a full     │                    │
      │  │      │  buffer of mic samples ready.    │                    │
      │  │      └──────────┬───────────────────────┘                    │
      │  │                 │                                            │
      │  │                 ▼                                            │
      │  │      ┌──────────────────────────────────┐                    │
      │  │      │  3b. Apply gain                  │                    │
      │  │      │                                  │                    │
      │  │      │  for each int16 sample:          │                    │
      │  │      │    amplified = sample × gain     │                    │
      │  │      │    clamp to INT16 range          │                    │
      │  │      │                                  │                    │
      │  │      │  (skipped if GAIN_SCALING_ENABLED│                    │
      │  │      │   = 0 at compile time)           │                    │
      │  │      └──────────┬───────────────────────┘                    │
      │  │                 │                                            │
      │  │                 ▼                                            │
      │  │      ┌──────────────────────────────────┐                    │
      │  │      │  3c. Write to stream buffer      │                    │
      │  │      │                                  │                    │
      │  │      │  sent = xStreamBufferSend(       │                    │
      │  │      │      stream, buf, bytes_read,    │                    │
      │  │      │      timeout=100ms)              │                    │
      │  │      │                                  │                    │
      │  │      │  If buffer full:                 │                    │
      │  │      │    sent < bytes_read             │                    │
      │  │      │    → excess bytes discarded      │                    │
      │  │      │    → warning logged              │                    │
      │  │      │                                  │                    │
      │  │      │  s_captured_bytes += sent        │                    │
      │  │      └──────────┬───────────────────────┘                    │
      │  │                 │                                            │
      │  │                 └──── loop ─────                             │
      │  │                                                              │
      │  └──────────────────────────────────────────────────────────────┘
      │
      │  ┌──────────────────────────────────────────────────────────────┐
      │  │  PHASE 4: Disable I2S RX                                     │
      │  │                                                              │
      │  │  i2s_channel_disable(s_rx_handle)                            │
      │  │    → BCLK and WS clocks stop                                 │
      │  │    → DMA halted                                              │
      │  │    → Mic goes idle                                           │
      │  │    → s_i2s_enabled = false                                   │
      │  │                                                              │
      │  │  (skipped in MIC_SIMULATE mode)                              │
      │  └──────────────────────────────────────────────────────────────┘
      │
      └── } // back to Phase 1 — wait for next mic_start() call
```

**The buffer filling during recording:**

```
  Buffer filling over time (16 KB total, 32 KB/s capture rate):

  Time  0ms:  [                                ] 0 KB    state=RECORDING
  Time 50ms:  [██                              ] 1.6 KB  capturing...
  Time 100ms: [████                            ] 3.2 KB  capturing...
  Time 200ms: [████████                        ] 6.4 KB  capturing...
  Time 300ms: [████████████                    ] 9.6 KB  capturing...
  Time 400ms: [████████████████                ] 12.8 KB capturing...
  Time 500ms: [████████████████████████████████] 16 KB ─► FULL
  Time 501ms: [████████████████████████████████] 16 KB   ⚠ samples discarded
       ...     (buffer stays full, new data lost)
  Time Nms:   mic_stop() called ──────────────────────► state=IDLE
              capture task exits loop, disables I2S

  At 16 kHz / 16-bit / mono (32 KB/s):
    16 KB buffer fills in ~500 ms.
    For longer recordings, increase RING_BUFFER_SIZE:
      32 KB  → 1.0 s       64 KB  → 2.0 s
      128 KB → 4.0 s       160 KB → 5.0 s
```

---

### Step 3: Stop Recording — `mic_stop()`

The voice assistant calls `mic_stop()` when the push-to-talk button is
released. This sets the state to IDLE, which causes the capture task to
exit its inner loop and disable I2S. Critically, the stream buffer is
NOT flushed — the captured PCM data stays in the buffer for upload.

```
  voice_assistant_task (on button release)
  │
  └── mic_stop()
      │
      │  1. If state != RECORDING → return (no-op)
      │
      │  2. state = IDLE
      │     → Capture task's inner loop condition becomes false
      │     → On next iteration, capture task exits loop
      │     → Capture task disables I2S RX (Phase 4)
      │
      │  3. vTaskDelay(50ms)
      │     → Wait for capture task to see IDLE and exit cleanly
      │     → After this, stream buffer is not being written to
      │
      │  4. Log: "Recording stopped — N bytes captured"
      │
      └── Returns
```

After `mic_stop()` returns:

```
  I2S RX channel: DISABLED (capture task ran Phase 4)
  Stream buffer: contains N bytes of captured PCM (NOT flushed)
  Capture task: sleeping in Phase 1, waiting for next RECORDING
  Upload task: does not exist
  State: IDLE
  Byte counter: s_captured_bytes = N (total bytes in buffer)
```

---

### Step 4: Upload Audio — `mic_upload()`

The voice assistant calls `mic_upload(url)` to send the captured audio to
the backend. This is a non-blocking call — it spawns an ephemeral upload
task and returns immediately.

```
  voice_assistant_task (after mic_stop)
  │
  └── mic_upload("http://backend.example.com/api/listen")
      │
      │  1. If s_captured_bytes == 0 → return (nothing to upload)
      │
      │  2. Copy URL to static buffer
      │     strncpy(s_upload_url, url, 511)
      │     (Caller's string may be on the stack — must copy)
      │
      │  3. state = UPLOADING
      │
      │  4. xTaskCreate(mic_upload_task, "mic_upload", ...)
      │     → Spawns the upload consumer task at priority 5
      │     → Saves task handle for stop/cleanup
      │
      └── Returns immediately (non-blocking)
```

**What the upload task does:**

```
  mic_upload_task()
  │
  │  ┌─────────────────────────────────────────────────────────┐
  │  │  STEP 1: Configure HTTP client                          │
  │  │                                                         │
  │  │  esp_http_client_init()                                 │
  │  │    → Method: POST                                       │
  │  │    → URL: copied from s_upload_url                      │
  │  │    → Timeout: 30 seconds                                │
  │  │                                                         │
  │  │  Set headers:                                           │
  │  │    Content-Type: application/octet-stream               │
  │  │    X-Audio-Format: pcm;rate=16000;bits=16;channels=1    │
  │  │                                                         │
  │  │  esp_http_client_open(client, s_captured_bytes)         │
  │  │    → content_length = total captured bytes              │
  │  │    → Backend knows exactly how much PCM to expect       │
  │  └─────────────────────────────────────────────────────────┘
  │
  │  ┌─────────────────────────────────────────────────────────┐
  │  │  STEP 2: Write buffer contents to HTTP body             │
  │  │                                                         │
  │  │  while (total_sent < s_captured_bytes) {                │
  │  │      if (state == IDLE) break; // cancelled             │
  │  │                                                         │
  │  │      received = xStreamBufferReceive(                   │
  │  │          stream, write_buf, 1024, timeout=1000ms)       │
  │  │      if (received == 0) break; // buffer drained early  │
  │  │                                                         │
  │  │      written = esp_http_client_write(                   │
  │  │          client, write_buf, received)                   │
  │  │      if (written < 0) → ERROR                           │
  │  │                                                         │
  │  │      total_sent += written                              │
  │  │  }                                                      │
  │  └─────────────────────────────────────────────────────────┘
  │
  │  ┌─────────────────────────────────────────────────────────┐
  │  │  STEP 3: Read response status                           │
  │  │                                                         │
  │  │  esp_http_client_fetch_headers(client)                  │
  │  │    → Expects HTTP 200 OK                                │
  │  │    → If status != 200 → state = ERROR                   │
  │  └─────────────────────────────────────────────────────────┘
  │
  │  ┌─────────────────────────────────────────────────────────┐
  │  │  STEP 4: Cleanup and state transition                   │
  │  │                                                         │
  │  │  state = DONE                                           │
  │  │  vTaskDelay(50ms)     ← window for voice_asst to see    │
  │  │  state = IDLE         ← ready for next recording        │
  │  │                                                         │
  │  │  esp_http_client_close(client)                          │
  │  │  esp_http_client_cleanup(client)                        │
  │  │  s_upload_task_handle = NULL                            │
  │  │  vTaskDelete(NULL)    ← task self-destructs             │
  │  └─────────────────────────────────────────────────────────┘
  │
  └── Task gone. Capture task is sleeping. System ready for next cycle.
```

**Comparison with speaker's network task:**

```
  Speaker network task:              Mic upload task:
  ─────────────────────              ────────────────

  HTTP POST (empty body)             HTTP POST (PCM body)
  reads response body (PCM)          writes request body (PCM)
  → xStreamBufferSend()             ← xStreamBufferReceive()
  runs concurrently with playback    runs AFTER recording is done
  state: BUFFERING → PLAYING         state: UPLOADING → DONE → IDLE
         → DRAINING

  The speaker streams in real-time   The mic uploads a complete
  (HTTP response → buffer → I2S).    recording (buffer → HTTP body).
```

---

## Chronological Event Log — Normal Push-to-Talk Cycle

This is the full sequence of events during one push-to-talk conversation,
listed in the order they happen. Each event shows who does what, what
state changes, and what hardware is active.

```
  ── t0: IDLE (waiting for user) ──────────────────────────────────────

  voice_assistant_task:  polling button_pressed() every 20ms (sleeping 99.95%)
  mic_capture_task:      sleeping — polling for RECORDING state every 20ms
  mic_upload_task:       does not exist
  speaker_network_task:  does not exist
  speaker_playback_task: sleeping — polling for PLAYING state every 20ms
  mic state:             IDLE
  speaker state:         IDLE
  I2S RX:                disabled
  I2S TX:                disabled

  ── t1: USER PRESSES BUTTON ─────────────────────────────────────────

  voice_assistant_task:  button_pressed() returns true → exits poll loop
  voice_assistant_task:  calls mic_start()
    mic_start():           resets stream buffer (xStreamBufferReset)
    mic_start():           s_captured_bytes = 0
    mic_start():           state → RECORDING
  mic state:             RECORDING

  ── t2: CAPTURE BEGINS (within 20ms of t1) ──────────────────────────

  mic_capture_task:      sees state == RECORDING → exits sleep loop
  mic_capture_task:      calls i2s_channel_enable(rx_handle)
  I2S RX:                enabled — BCLK/WS clocks start, mic outputs samples
  mic_capture_task:      enters capture loop:
                           i2s_channel_read() → blocks until DMA has 1024 bytes
                           apply_gain() → multiply samples by gain factor
                           xStreamBufferSend() → write to buffer
                           s_captured_bytes += bytes written
                           repeat...
  voice_assistant_task:  polling button_pressed() every 20ms (waiting for release)

  ── t3: USER RELEASES BUTTON ────────────────────────────────────────

  voice_assistant_task:  button_pressed() returns false → exits poll loop
  voice_assistant_task:  calls mic_stop()
    mic_stop():            state → IDLE
    mic_stop():            vTaskDelay(50ms) — waits for capture task to exit
  mic_capture_task:      sees state != RECORDING → exits capture loop
  mic_capture_task:      calls i2s_channel_disable(rx_handle)
  I2S RX:                disabled — clocks stop, mic goes idle
  mic_capture_task:      returns to Phase 1 sleep loop
  mic state:             IDLE (buffer contains s_captured_bytes of PCM)

  ── t4: UPLOAD STARTS ────────────────────────────────────────────────

  voice_assistant_task:  calls mic_upload(BACKEND_MIC_URL)
    mic_upload():          copies URL to static buffer
    mic_upload():          state → UPLOADING
    mic_upload():          xTaskCreate(mic_upload_task, pri 5) — spawns task
    mic_upload():          returns immediately (non-blocking)
  mic state:             UPLOADING

  ── t5: UPLOAD IN PROGRESS ──────────────────────────────────────────

  mic_upload_task:       esp_http_client_init() → configure POST request
  mic_upload_task:       set headers: Content-Type, X-Audio-Format
  mic_upload_task:       esp_http_client_open(content_length = N)
  mic_upload_task:       loop: xStreamBufferReceive() → esp_http_client_write()
                           (reads buffer in 1024-byte chunks, writes to HTTP body)
                           (blocks on TCP send — 0% CPU while kernel sends)
  voice_assistant_task:  polling mic_get_state() every 100ms (waiting for IDLE)

  ── t6: UPLOAD COMPLETE ─────────────────────────────────────────────

  mic_upload_task:       all bytes sent → esp_http_client_fetch_headers()
  mic_upload_task:       checks status code (expects HTTP 200)
  mic_upload_task:       state → DONE
  mic_upload_task:       vTaskDelay(50ms)
  mic_upload_task:       state → IDLE
  mic_upload_task:       esp_http_client_close() + cleanup
  mic_upload_task:       vTaskDelete(NULL) — self-destructs
  mic_upload_task:       no longer exists
  voice_assistant_task:  mic_get_state() returns IDLE → exits poll loop
  mic state:             IDLE

  ── t7: PLAYBACK STARTS ─────────────────────────────────────────────

  voice_assistant_task:  calls speaker_play_url(BACKEND_SPEAKER_URL)
    speaker_play_url():    resets speaker stream buffer
    speaker_play_url():    state → BUFFERING
    speaker_play_url():    xTaskCreate(speaker_network_task, pri 5)
    speaker_play_url():    returns immediately (non-blocking)
  speaker state:         BUFFERING
  voice_assistant_task:  polling speaker_get_state() every 200ms

  ── t8: SPEAKER NETWORK FETCHES AUDIO ───────────────────────────────

  speaker_network_task:  HTTP POST to backend
  speaker_network_task:  esp_http_client_read() → BLOCKS on recv()
                           (backend is processing: STT → LLM → TTS → transcode)
                           (task suspended by OS, 0% CPU while waiting)
                           (may block for 1–10 seconds)
  speaker_network_task:  backend starts streaming PCM chunks
  speaker_network_task:  xStreamBufferSend() → fills speaker buffer
  speaker_network_task:  buffer ≥ prebuffer threshold → state → PLAYING
  speaker state:         PLAYING

  ── t9: SPEAKER PLAYBACK ACTIVE ─────────────────────────────────────

  speaker_playback_task: sees state == PLAYING → exits sleep loop
  speaker_playback_task: i2s_channel_enable(tx_handle)
  I2S TX:                enabled — BCLK/WS clocks start, DMA feeds amp
  speaker_playback_task: enters playback loop:
                           xStreamBufferReceive() → read PCM from buffer
                           apply_volume() → scale samples by volume factor
                           i2s_channel_write() → blocks until DMA accepts
                           repeat...
  speaker_network_task:  continues filling buffer concurrently
  voice_assistant_task:  still polling speaker_get_state() every 200ms

  ── t10: HTTP TRANSFER COMPLETE ──────────────────────────────────────

  speaker_network_task:  esp_http_client_read() returns 0 (end of stream)
  speaker_network_task:  state → DRAINING
  speaker_network_task:  esp_http_client_close() + cleanup
  speaker_network_task:  vTaskDelete(NULL) — self-destructs
  speaker_network_task:  no longer exists
  speaker state:         DRAINING

  ── t11: BUFFER DRAINED ─────────────────────────────────────────────

  speaker_playback_task: xStreamBufferReceive() returns 0 (buffer empty)
  speaker_playback_task: state == DRAINING + empty → state → STOPPED
  speaker_playback_task: i2s_channel_disable(tx_handle)
  I2S TX:                disabled — clocks stop, amp goes quiet
  speaker_playback_task: state → IDLE
  speaker_playback_task: returns to Phase 1 sleep loop
  speaker state:         IDLE

  ── t12: CYCLE COMPLETE ─────────────────────────────────────────────

  voice_assistant_task:  speaker_get_state() returns IDLE → exits poll loop
  voice_assistant_task:  vTaskDelay(200ms) — brief pause
  voice_assistant_task:  loops back to t0 — waiting for next button press
```

---

## Data Flow Through the Stream Buffer

The stream buffer handles the timing difference between I2S capture
(steady) and HTTP upload (bursty). Unlike the speaker driver where
network and I2S run concurrently, the mic driver operates in two
sequential phases: fill (recording), then drain (upload).

```
  Phase 1 — RECORDING (I2S → buffer):

  I2S delivers data at a steady 32 KB/s:
  ┌──────────────────────────────────────────────────────────────────┐
  │  ████████████████████████████████████████████████████████████████│
  │  constant rate, 1024 bytes every ~32 ms                          │
  └──────────────────────────────────────────────────────────────────┘

  Buffer fills steadily until mic_stop() is called:
  ┌──────────────────────────────────────────────────────────────────┐
  │                                                                  │
  │  Fill level over time:                                           │
  │                                                                  │
  │  100% ┤                        ╭─────────────── mic_stop()       │
  │       │                       ╱                                  │
  │   75% ┤                      ╱                                   │
  │       │                     ╱                                    │
  │   50% ┤                    ╱                                     │
  │       │                   ╱                                      │
  │   25% ┤                  ╱                                       │
  │       │                 ╱                                        │
  │    0% ┤────────────────╱                                         │
  │       └──┬────┬────┬────┬────┬────┬────┬────                     │
  │         0ms  100  200  300  400  500  stop                       │
  │                                                                  │
  │  Linear fill at 32 KB/s. If buffer is 16 KB, it fills in ~500ms. │
  │  If user holds button longer than that, new samples are lost.    │
  └──────────────────────────────────────────────────────────────────┘


  Phase 2 — UPLOADING (buffer → HTTP):

  Upload drains buffer in chunks via HTTP write:
  ┌──────────────────────────────────────────────────────────────────┐
  │                                                                  │
  │  Fill level over time:                                           │
  │                                                                  │
  │  100% ┤──╮                                                       │
  │       │   ╲                                                      │
  │   75% ┤    ╲                                                     │
  │       │     ╲   ╭─ brief stall (network jitter)                  │
  │   50% ┤      ╲──╯                                                │
  │       │        ╲                                                 │
  │   25% ┤         ╲                                                │
  │       │          ╲                                               │
  │    0% ┤           ╲─── empty → DONE → IDLE                       │
  │       └──┬────┬────┬────┬────┬────                               │
  │         0ms  50  100  150  200                                   │
  │                                                                  │
  │  Drain speed depends on network. On a local WiFi network with    │
  │  good throughput, 16 KB uploads in <200ms. The 1-second HTTP     │
  │  timeout on xStreamBufferReceive protects against stalls.        │
  └──────────────────────────────────────────────────────────────────┘
```

---

## Gain Scaling Path

Software gain is applied by the capture task between the I2S read and the
buffer write. It's the only processing done on the PCM data — the ESP32
sends raw samples to the backend.

```
  Raw PCM from I2S                    After gain scaling (gain = 2.0)
  ┌──────────────────────┐           ┌──────────────────────┐
  │ Sample 0:  +4096     │  × 2.0 ──►│ Sample 0:  +8192     │
  │ Sample 1:  +16384    │  × 2.0 ──►│ Sample 1:  +32767    │ ← clamped!
  │ Sample 2:  -2000     │  × 2.0 ──►│ Sample 2:  -4000     │
  │ Sample 3:  +0        │  × 2.0 ──►│ Sample 3:  +0        │
  │ Sample 4:  -20000    │  × 2.0 ──►│ Sample 4:  -32768    │ ← clamped!
  │ ...                  │           │ ...                  │
  └──────────────────────┘           └──────────────────────┘
                                          │
                                          ▼
                                     xStreamBufferSend()

  Controlled by:
    mic_set_gain(2.0f)       → s_mic_cfg.gain = 2.0
    GAIN_SCALING_ENABLED=0   → bypass entirely (no multiply, no clamp)
    DEFAULT_GAIN=1.0         → unity (effective no-op at compile default)

  Clipping protection:
    int32_t amplified = (int32_t)(sample × gain);
    if (amplified > +32767)  amplified = +32767;   // INT16_MAX
    if (amplified < -32768)  amplified = -32768;   // INT16_MIN

  Comparison with speaker volume:
    Speaker: volume  × sample  (0.0–1.0, attenuates)
    Mic:     gain    × sample  (0.0–∞,   amplifies — with clipping guard)
```

---

## Push-to-Talk Integration — Voice Assistant

The voice assistant (`voice_assistant.c`) is the conversation coordinator.
It handles the push-to-talk button, sequences the mic and speaker drivers,
and connects both to the backend. The mic driver itself has no knowledge
of buttons — it just exposes `mic_start()` and `mic_stop()`.

### Button Configuration

```
  Physical wiring:

    GPIO4 (D3 on XIAO)  ────────┐
                                │
                            ┌───┴───┐
                            │ Button│   Momentary switch
                            │  (NO) │   (normally open)
                            └───┬───┘
                                │
                              GND ──────


  GPIO configuration (voice_assistant_init):

    Mode:      GPIO_MODE_INPUT
    Pull-up:   GPIO_PULLUP_ENABLE     ← pin is HIGH when button not pressed
    Pull-down: GPIO_PULLDOWN_DISABLE
    Interrupt: GPIO_INTR_DISABLE      ← polling, not interrupt-driven

  Reading:
    gpio_get_level(GPIO4) == 0  → button PRESSED  (pulled to GND)
    gpio_get_level(GPIO4) == 1  → button RELEASED  (pulled HIGH)
```

### Complete Conversation Flow

```
  voice_assistant_task()
  │
  ├── 1. network_app_wait_for_connection()
  │       └── blocks until WiFi/PPP has IP
  │
  └── 2. Loop forever:
      │
      ├── a. Poll for button press (20ms interval)
      │       while (gpio_get_level(4) != 0) vTaskDelay(20ms);
      │
      ├── b. mic_start()
      │       → resets buffer, state = RECORDING
      │       → capture task begins filling buffer
      │
      ├── c. Poll for button release (20ms interval)
      │       while (gpio_get_level(4) == 0) vTaskDelay(20ms);
      │
      ├── d. mic_stop()
      │       → state = IDLE, capture task exits, I2S disabled
      │       → PCM data preserved in stream buffer
      │
      ├── e. mic_upload(BACKEND_MIC_URL)
      │       → state = UPLOADING, spawns upload task
      │       → returns immediately
      │
      ├── f. Poll for upload complete (100ms interval)
      │       while (state != IDLE && state != ERROR) vTaskDelay(100ms);
      │       → if ERROR, log warning, delay 1s, continue to top
      │
      ├── g. speaker_play_url(BACKEND_SPEAKER_URL)
      │       → state = BUFFERING, spawns speaker network task
      │       → returns immediately
      │
      ├── h. Poll for playback complete (200ms interval)
      │       while (state != IDLE && state != ERROR) vTaskDelay(200ms);
      │
      ├── i. vTaskDelay(200ms)
      │       → brief pause before listening for next button press
      │
      └── loop back to step a
```

### End-to-End Interaction Sequence

```
  User        voice_assistant     mic_driver         speaker_driver    backend
  ────        ───────────────     ──────────         ──────────────    ───────

  press ─────► button detected
               │
               ├── mic_start() ──► state = RECORDING
               │                   capture task:
               │                     enable I2S RX
               │                     read samples
  hold         │                     apply gain
  (speaking)   │                     fill buffer
               │                     ...
               │
  release ────► button detected
               │
               ├── mic_stop() ───► state = IDLE
               │                   capture task:
               │                     exit loop
               │                     disable I2S RX
               │                   buffer: N bytes of PCM
               │
               ├── mic_upload() ─► state = UPLOADING
               │                   upload task spawned:
               │                     POST /api/listen
               │                     Content-Length: N
               │                     X-Audio-Format: pcm;...
               │                     Body: [N bytes PCM] ──────────► received
               │                     ...                               │
               │                                                       │ STT
               │                     ◄── HTTP 200 OK                   │ (speech
               │                   state = DONE → IDLE                 │  to text)
               │                   upload task self-deletes            │
               │                                                       │ LLM
               │   mic IDLE ◄──── complete                             │ (generate
               │                                                       │  response)
               │                                                       │
               ├── speaker_play_url() ───────────────────────────────  │ TTS
               │    └──► speaker network task spawned                  │ (text to
               │         POST /api/speak                               │  speech)
               │         (connection stays open)                       │
               │         recv() BLOCKS ──────────────────────────────  │ transcode
               │         (0% CPU, waiting for backend)                 │ to PCM
               │         ...                                           │
               │         ...                                           ▼
               │         ◄────────────────────── chunk: [1024 PCM bytes]
               │         writes to speaker buffer
               │         pre-buffer threshold → PLAYING
               │         playback task: buffer → volume → I2S TX → amp → speaker
               │         ...
               │         ◄────────────────────── chunk: [512 PCM bytes]
               │         ◄────────────────────── chunk: 0 (end)
               │         state = DRAINING
               │         speaker network task self-deletes
               │
               │         playback task drains buffer → STOPPED → IDLE
               │
               │   speaker IDLE
               │
               └── loop (wait for next button press)
```

---

## I2S Peripheral Modes

The mic driver supports two I2S configurations, selectable at compile time
via `MIC_I2S_NUM`. Both are functionally identical after initialization —
the capture task code is the same either way.

### Mode 1: Separate I2S1 (default)

```
  MIC_I2S_NUM = 1

  ESP32-S3                         Devices
  ────────                         ───────

  I2S0 (speaker — unchanged):
    BCLK (GPIO9)  ─────────────── MAX98357A BCLK
    WS   (GPIO10) ─────────────── MAX98357A LRC
    DOUT (GPIO44) ─────────────── MAX98357A DIN

  I2S1 (mic — independent):
    BCLK (GPIO1)  ─────────────── INMP441 SCK
    WS   (GPIO2)  ─────────────── INMP441 WS
    DIN  (GPIO3)  ◄──────────────  INMP441 SD


  Init code (mic_driver.c):
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, NULL, &s_rx_handle);  // RX only

  Pros: zero coordination with speaker, simplest code
  Cons: uses 3 GPIO pins for mic
```

### Mode 2: Full-Duplex I2S0 (shared with speaker)

```
  MIC_I2S_NUM = 0

  ESP32-S3                         Devices
  ────────                         ───────

  I2S0 (shared — full-duplex):
    BCLK (GPIO9)  ────────────┬── MAX98357A BCLK
                              └── INMP441 SCK
    WS   (GPIO10) ────────────┬── MAX98357A LRC
                              └── INMP441 WS
    DOUT (GPIO44) ─────────────── MAX98357A DIN    (ESP → speaker)
    DIN  (GPIO1)  ◄──────────────  INMP441 SD       (mic → ESP)


  Coordination sequence:

    speaker_driver.c                    mic_driver.c
    ────────────────                    ────────────
    (SPEAKER_FULL_DUPLEX=1)             (MIC_I2S_NUM=0)

    speaker_init():                     mic_init():
      i2s_new_channel(I2S_NUM_0,          s_rx_handle =
        &s_tx_handle,                       speaker_get_rx_handle();
        &s_rx_handle_fd);                 i2s_channel_init_std_mode(
      // creates BOTH handles               s_rx_handle, &rx_config);
                                          // configures RX for mic GPIO

  After init: both drivers use their own handle independently.
  No further coordination needed.

  Pros: saves 2 GPIO pins (only 1 new pin needed)
  Cons: speaker_init must run before mic_init
```

---

## Simulate Mode — `MIC_SIMULATE`

When `MIC_SIMULATE=1`, all I2S hardware calls are replaced with simulated
equivalents. This enables testing on Wokwi or without physical hardware.

```
  MIC_SIMULATE = 0 (default):         MIC_SIMULATE = 1:
  ─────────────────────────            ──────────────────

  mic_init():
    i2s_new_channel()                  (skipped — log message instead)
    i2s_channel_init_std_mode()        (skipped)

  mic_capture_task Phase 2:
    i2s_channel_enable()               (skipped)

  mic_capture_task Phase 3:
    i2s_channel_read(buf, 1024)        memset(buf, 0, 1024)  ← silence
                                       vTaskDelay(32ms)      ← real-time rate
    (blocks on DMA)                    (blocks on timer)

  mic_capture_task Phase 4:
    i2s_channel_disable()              (skipped)


  Simulated timing calculation:
    delay_ms = (CAPTURE_BUF_SIZE × 1000) / (sample_rate × bytes_per_sample × channels)
    delay_ms = (1024 × 1000) / (16000 × 2 × 1)
    delay_ms = 1024000 / 32000
    delay_ms = 32 ms per 1024-byte read

  This means the simulated capture task produces silence at the
  same rate that real I2S hardware would produce mic samples.
  The buffer fills, upload works, and the voice assistant loop
  functions identically — just with silent audio content.
```

---

## File Map

```
  main/
  ├── main.c                 3 lines added:  mic_init() + xTaskCreate + #include
  ├── mic_driver.h           Public API:     7 functions + config struct + state enum
  ├── mic_driver.c           Implementation: ~655 lines
  │   ├── Configuration      Audio preset, I2S peripheral, pin defines, buffer, gain
  │   ├── apply_gain()       Software gain scaling (16-bit PCM × float, with clipping)
  │   ├── mic_init()         I2S RX peripheral + stream buffer setup
  │   ├── mic_upload_task()  Ephemeral HTTP consumer task (buffer → POST body)
  │   ├── mic_get_state()    Public API: query state
  │   ├── mic_set_gain()     Public API: set gain [0.0, ∞)
  │   ├── mic_start()        Public API: begin recording
  │   ├── mic_stop()         Public API: stop recording (preserve buffer)
  │   ├── mic_upload()       Public API: upload captured audio to URL
  │   └── mic_capture_task() Permanent I2S producer task
  ├── speaker_driver.c       Modified:  +SPEAKER_FULL_DUPLEX support (conditional)
  ├── speaker_driver.h       Unchanged
  ├── voice_assistant.c      Rewritten: push-to-talk loop (mic → upload → speaker)
  ├── voice_assistant.h      Updated:   docs reflect mic integration
  └── CMakeLists.txt         Added:     "mic_driver.c"
```

---

## Task Priority Map

```
  Priority 6:  speaker_playback_task   (permanent, driver-internal)
                 └── I2S TX consumer — always gets CPU for audio output
               mic_capture_task        (permanent, driver-internal)
                 └── I2S RX producer — always gets CPU for audio input

  Priority 5:  voice_assistant_task    (permanent, application layer)
                 └── button poll, sequences mic + speaker
               mic_upload_task         (ephemeral, driver-internal)
                 └── HTTP upload — spawned per recording
               speaker_network_task    (ephemeral, driver-internal)
                 └── HTTP fetch — spawned per playback

  Priority 4:  network_app_task        (existing)
               test_network_task       (existing)

  Both hardware tasks (mic capture + speaker playback) run at priority 6.
  They never compete: one reads I2S RX while the other writes I2S TX.
  When both are active (not the case in push-to-talk, but possible
  in other modes), they service different peripherals.

  The voice assistant and ephemeral tasks all run at priority 5.
  They cooperate through state polling (vTaskDelay loops), never
  running simultaneously for the same driver.
```

---

## Memory Budget

```
  ┌─────────────────────────┬────────────┬──────────────────────────┐
  │ Component               │ RAM Usage  │ Notes                    │
  ├─────────────────────────┼────────────┼──────────────────────────┤
  │ Stream buffer           │ 16,384 B   │ Configurable             │
  │ I2S DMA buffers         │ ~4,096 B   │ Managed by I2S driver    │
  │ Capture task stack      │  4,096 B   │ Standard task stack      │
  │ Upload task stack       │  4,096 B   │ HTTP client needs stack  │
  │ I2S RX channel handle   │    ~200 B  │ Internal ESP-IDF         │
  ├─────────────────────────┼────────────┼──────────────────────────┤
  │ Mic driver total        │  ~29 KB    │ ~5.6% of 512 KB SRAM     │
  │ + Speaker driver        │  ~29 KB    │ Already allocated        │
  ├─────────────────────────┼────────────┼──────────────────────────┤
  │ Combined audio total    │  ~58 KB    │ ~11% of SRAM             │
  └─────────────────────────┴────────────┴──────────────────────────┘
```

---

## Configuration Quick Reference

All compile-time settings are `#define`s at the top of `mic_driver.c`:

| Define | Default | What it controls |
|--------|---------|------------------|
| `AUDIO_PRESET` | `AUDIO_PRESET_SPEECH` | `SPEECH` = 16kHz/16-bit/mono, `HIFI` = 48kHz/16-bit/mono |
| `MIC_I2S_NUM` | `1` | `0` = full-duplex on I2S0 (shared with speaker), `1` = separate I2S1 |
| `MIC_I2S_BCLK_PIN` | `1` or `9` | I2S bit clock GPIO (depends on I2S mode) |
| `MIC_I2S_WS_PIN` | `2` or `10` | I2S word select GPIO (depends on I2S mode) |
| `MIC_I2S_DIN_PIN` | `3` or `1` | I2S data input GPIO (mic's SD/DOUT pin) |
| `RING_BUFFER_SIZE` | `16384` (16 KB) | Stream buffer size = max recording duration |
| `GAIN_SCALING_ENABLED` | `1` | `1` = apply software gain, `0` = bypass |
| `DEFAULT_GAIN` | `1.0f` | Initial gain (1.0 = unity, >1.0 = amplify) |
| `MIC_SIMULATE` | `0` | `1` = generate silence instead of real I2S reads |

Voice assistant settings in `voice_assistant.c`:

| Define | Default | What it controls |
|--------|---------|------------------|
| `MIC_ENABLED` | `1` | `1` = push-to-talk loop, `0` = speaker-only (original) |
| `PTT_BUTTON_ENABLED` | `1` | `1` = physical button, `0` = auto-record timer |
| `PTT_BUTTON_PIN` | `4` (GPIO4, D3) | Push-to-talk button GPIO |
| `PTT_AUTO_RECORD_MS` | `3000` | Auto-record duration when button is disabled |
| `BACKEND_MIC_URL` | `http://...` | Backend endpoint for mic audio upload |
| `BACKEND_SPEAKER_URL` | `http://...` | Backend endpoint for speaker audio response |

Speaker full-duplex setting in `speaker_driver.c`:

| Define | Default | What it controls |
|--------|---------|------------------|
| `SPEAKER_FULL_DUPLEX` | `0` | `1` = create TX+RX handles for mic sharing I2S0 |
