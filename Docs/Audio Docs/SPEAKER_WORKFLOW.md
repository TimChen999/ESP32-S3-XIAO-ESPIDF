# Speaker Driver — Workflow

This document walks through the speaker driver's complete lifecycle step by
step. Each section explains what happens conceptually, which code runs, and
how data moves between tasks, buffers, and hardware.

---

## High-Level Architecture

Two FreeRTOS tasks cooperate in a producer–consumer pattern, connected by a
shared stream buffer. The playback task is permanent (created once at boot);
the network task is ephemeral (created per playback request, self-deletes).

```
  ┌──────────────────────────────────────────────────────────────────────────┐
  │                          ESP32-S3 XIAO                                  │
  │                                                                          │
  │   ┌─────────────────────┐                 ┌───────────────────────────┐  │
  │   │  speaker_play_url() │                 │  speaker_playback_task()  │  │
  │   │  (caller — any task │                 │  (permanent, priority 6)  │  │
  │   │   with network)     │                 │                           │  │
  │   └────────┬────────────┘                 │  Phase 1: Sleep (poll)    │  │
  │            │ spawns                       │  Phase 2: Enable I2S      │  │
  │            ▼                              │  Phase 3: Read → Vol → TX │  │
  │   ┌────────────────────┐   stream buffer  │  Phase 4: Disable I2S     │  │
  │   │ speaker_network_   │  ┌────────────┐  │                           │  │
  │   │ task (ephemeral,   │  │ 16 KB ring │  │                           │  │
  │   │ priority 5)        ├──►  buffer    ├──►                           │  │
  │   │                    │  │            │  │                           │  │
  │   │ HTTP POST → read   │  └────────────┘  │  I2S write ──────────┐   │  │
  │   │ chunks → buffer    │                  │                      │   │  │
  │   └────────────────────┘                  └──────────────────────┼───┘  │
  │                                                                  │      │
  └──────────────────────────────────────────────────────────────────┼──────┘
                                                                     │
                                              BCLK (GPIO9) ──────────┤
                                              WS   (GPIO10) ─────────┤
                                              DOUT (GPIO44) ─────────┘
                                                                     │
                                              ┌──────────────────────▼──┐
                                              │  I2S Amplifier          │
                                              │  (MAX98357A / NS4168)   │
                                              │                         │
                                              │  Digital → Analog       │
                                              │  + Class D amp          │
                                              └──────────┬──────────────┘
                                                         │ analog audio
                                                      ┌──▼──┐
                                                      │ 🔊  │ Speaker
                                                      └─────┘
```

---

## State Machine

Every state transition is driven by exactly one actor (noted in parentheses).
The state variable lives in `s_speaker_cfg.state` and is read by both tasks.

```
                      speaker_init()
                           │
                           ▼
                    ┌─────────────┐
              ┌────►│    IDLE      │◄──────────────────────────────┐
              │     └──────┬──────┘                                │
              │            │ speaker_play_url()                    │
              │            │   (caller)                            │
              │            ▼                                       │
              │     ┌─────────────┐                                │
              │     │  BUFFERING  │                                │
              │     │             │                                │
              │     │ Network task│                                │
              │     │ fills buffer│                                │
              │     └──────┬──────┘                                │
              │            │ buffer ≥ prebuffer_threshold          │
              │            │   (network task)                      │
              │            ▼                                       │
              │     ┌─────────────┐                                │
              │     │  PLAYING    │                                │
              │     │             │                                │
              │     │ Both tasks  │                                │
              │     │ active:     │                                │
              │     │  net writes │                                │
              │     │  I2S reads  │                                │
              │     └──────┬──────┘                                │
              │            │ HTTP transfer complete                │
              │            │   (network task)                      │
              │            ▼                                       │
              │     ┌─────────────┐                                │
              │     │  DRAINING   │                                │
              │     │             │                                │
              │     │ Net done.   │                                │
              │     │ I2S plays   │                                │
              │     │ remaining   │                                │
              │     │ buffer.     │                                │
              │     └──────┬──────┘                                │
              │            │ buffer empty                          │
              │            │   (playback task)                     │
              │            ▼                                       │
              │     ┌─────────────┐                                │
              │     │  STOPPED    │────────────────────────────────┘
              │     └─────────────┘   (playback task → auto IDLE)
              │
              │  speaker_stop()
              │    (any task, any time)
              └────────────────────────

         ERROR ◄── fatal I2S error, HTTP failure, alloc failure
```

---

## Workflow Step by Step

### Step 0: Boot — `app_main()` in `main.c`

When the ESP32 boots, `app_main()` runs the one-time setup. The speaker
driver follows the same pattern as every other driver: one init call,
one task creation.

```
  app_main()
  │
  ├── esp_netif_init()                  TCP/IP stack
  ├── esp_event_loop_create_default()   event system
  ├── network_app_init()                IP/SNTP event handlers
  ├── wifi_driver_init()                Wi-Fi connect
  │
  ├── speaker_init()  ◄───────────── one init call
  ├── xTaskCreatePinnedToCore(         one task creation
  │       speaker_playback_task,
  │       "speaker",
  │       4096,                        stack size
  │       NULL,
  │       TASK_PRIORITY + 1,           priority 6 (higher than net tasks)
  │       NULL,
  │       DRIVER_CORE)                 pinned to core 1
  │
  ├── xTaskCreatePinnedToCore(network_app_task, ...)
  └── xTaskCreatePinnedToCore(test_network_task, ...)
```

**What `speaker_init()` does internally:**

```
  speaker_init()
  │
  │  ┌─────────────────────────────────────────────────────────┐
  │  │  I2S INITIALIZATION (skipped if SPEAKER_SIMULATE = 1)   │
  │  │                                                         │
  │  │  1. i2s_new_channel()                                   │
  │  │     → Creates an I2S TX channel handle on I2S_NUM_0     │
  │  │     → ESP32 is MASTER (generates clocks)                │
  │  │     → NULL for RX handle (speaker = output only)        │
  │  │                                                         │
  │  │  2. i2s_channel_init_std_mode()                         │
  │  │     → Philips standard I2S mode                         │
  │  │     → Clock: sample rate from config (16 kHz default)   │
  │  │     → Slots: 16-bit, mono or stereo                     │
  │  │     → GPIO: BCLK=9, WS=10, DOUT=44 (configurable)      │
  │  │     → MCLK unused, DIN unused                           │
  │  │                                                         │
  │  │  3. Channel is NOT enabled yet                          │
  │  │     → No clocks toggling, no power to amp               │
  │  │     → Enabled later by the playback task                │
  │  └─────────────────────────────────────────────────────────┘
  │
  │  ┌─────────────────────────────────────────────────────────┐
  │  │  STREAM BUFFER ALLOCATION                               │
  │  │                                                         │
  │  │  xStreamBufferCreate(16384, 1)                          │
  │  │     → 16 KB ring buffer (500 ms of audio at 16 kHz)    │
  │  │     → Trigger level = 1 byte (wake on any data)         │
  │  │                                                         │
  │  │  If allocation fails → state = ERROR, return            │
  │  └─────────────────────────────────────────────────────────┘
  │
  └── state = IDLE
```

After `speaker_init()` returns, the system is in this state:

```
  I2S channel: exists, configured, DISABLED (no clocks)
  Stream buffer: allocated, empty
  Playback task: running, sleeping in Phase 1 (polling for PLAYING)
  Network task: does not exist
  State: IDLE
```

---

### Step 1: Trigger Playback — `speaker_play_url()`

Any task with network access calls `speaker_play_url(url)`. This is a
non-blocking call — it returns immediately after spawning the network task.

```
  Caller (e.g. voice_assistant_task)
  │
  └── speaker_play_url("http://backend.example.com/api/speak")
      │
      │  1. If already playing → speaker_stop() first
      │
      │  2. Copy URL to static buffer
      │     strncpy(s_play_url, url, 511)
      │     (The caller's string might be on the stack — must copy)
      │
      │  3. Reset stream buffer
      │     xStreamBufferReset(s_audio_stream)
      │
      │  4. state = BUFFERING
      │
      │  5. xTaskCreate(speaker_network_task, "spk_net", ...)
      │     → Spawns the network producer task at priority 5
      │     → Saves task handle for stop/cleanup
      │
      └── Returns immediately (non-blocking)
```

After `speaker_play_url()` returns:

```
  I2S channel: configured, DISABLED
  Stream buffer: empty
  Playback task: still sleeping (polling, sees BUFFERING, keeps waiting)
  Network task: just created, about to start HTTP connection
  State: BUFFERING
```

---

### Step 2: Network Fetches Audio — `speaker_network_task()`

The network task is the **producer**. It connects to the backend, reads the
HTTP response body (raw PCM), and pushes bytes into the stream buffer.

```
  speaker_network_task()
  │
  │  ┌─────────────────────────────────────────────────────────┐
  │  │  STEP 1: Configure HTTP client                          │
  │  │                                                         │
  │  │  esp_http_client_init()                                 │
  │  │    → Method: POST                                       │
  │  │    → URL: copied from s_play_url                        │
  │  │    → Timeout: 30 seconds                                │
  │  │                                                         │
  │  │  Set headers:                                           │
  │  │    Content-Type: application/json                        │
  │  │    X-Audio-Format: pcm;rate=16000;bits=16;channels=1    │
  │  └─────────────────────────────────────────────────────────┘
  │
  │  ┌─────────────────────────────────────────────────────────┐
  │  │  STEP 2: Open connection and send request               │
  │  │                                                         │
  │  │  esp_http_client_open(client, 0)                        │
  │  │    → content_length = 0 (no request body for now)       │
  │  │                                                         │
  │  │  TODO: When mic driver exists, the request body will    │
  │  │  contain recorded PCM from the microphone:              │
  │  │    1. Mic captures audio → buffer                       │
  │  │    2. This task sends mic PCM as POST body              │
  │  │    3. Backend: STT → LLM → TTS → PCM response          │
  │  └─────────────────────────────────────────────────────────┘
  │
  │  ┌─────────────────────────────────────────────────────────┐
  │  │  STEP 3: Read response headers                          │
  │  │                                                         │
  │  │  esp_http_client_fetch_headers(client)                  │
  │  │    → Expects HTTP 200 OK                                │
  │  │    → content_length = -1 means chunked transfer         │
  │  │                                                         │
  │  │  If status != 200 → state = ERROR, cleanup, exit        │
  │  └─────────────────────────────────────────────────────────┘
  │
  │  ┌─────────────────────────────────────────────────────────┐
  │  │  STEP 4: Stream PCM into buffer (main loop)             │
  │  │                                                         │
  │  │  while (1) {                                            │
  │  │      if (state == IDLE) break; // stop requested        │
  │  │                                                         │
  │  │      read_len = esp_http_client_read(buf, 1024)         │
  │  │      if (read_len == 0) break; // end of stream         │
  │  │      if (read_len < 0) → ERROR                          │
  │  │                                                         │
  │  │      xStreamBufferSend(stream, buf, read_len, 5000ms)   │
  │  │        → blocks if buffer full (back-pressure)          │
  │  │                                                         │
  │  │      if (buffer_bytes ≥ prebuffer_threshold)            │
  │  │          state = PLAYING  ◄── wakes up playback task    │
  │  │  }                                                      │
  │  └─────────────────────────────────────────────────────────┘
  │
  │  ┌─────────────────────────────────────────────────────────┐
  │  │  STEP 5: Signal end-of-stream                           │
  │  │                                                         │
  │  │  state = DRAINING                                       │
  │  │    → Tells playback task: no more data coming,          │
  │  │      finish what's in the buffer.                       │
  │  └─────────────────────────────────────────────────────────┘
  │
  │  cleanup: close and free HTTP client
  │  s_network_task_handle = NULL
  │  vTaskDelete(NULL)  → task self-destructs
  │
  └── Task gone. Playback task continues independently.
```

**The pre-buffer mechanism in detail:**

```
  Buffer filling over time (16 KB total, 4 KB = 25% threshold):

  Time  0ms:  [                                ] 0 KB    state=BUFFERING
  Time 50ms:  [██                              ] 1 KB    state=BUFFERING
  Time 100ms: [████                            ] 2 KB    state=BUFFERING
  Time 150ms: [██████                          ] 3 KB    state=BUFFERING
  Time 200ms: [████████                        ] 4 KB ─► state=PLAYING ◄─ threshold hit
  Time 250ms: [██████████                      ] 5 KB    playback task consuming now
  Time 300ms: [████████████                    ] 5 KB    net writes + I2S reads
       ...     steady state: buffer fluctuates
  Time 2000ms:[██████                          ] 3 KB    still streaming
       ...
  Time 5000ms:[████                            ] 2 KB ─► state=DRAINING (HTTP done)
  Time 5100ms:[██                              ] 1 KB    playback draining
  Time 5200ms:[                                ] 0 KB ─► state=STOPPED → IDLE
```

---

### Step 3: Playback Consumes Audio — `speaker_playback_task()`

The playback task is the **consumer**. It runs forever, cycling between
sleeping (waiting for audio) and actively playing.

```
  speaker_playback_task()      (created once at boot, runs forever)
  │
  └── while (1) {                      outer loop — repeats per playback session
      │
      │  ┌──────────────────────────────────────────────────────────────┐
      │  │  PHASE 1: Wait for PLAYING state                            │
      │  │                                                              │
      │  │  while (state != PLAYING && state != DRAINING)              │
      │  │      vTaskDelay(20ms);                                       │
      │  │                                                              │
      │  │  The task polls every 20ms. Low overhead: ~0.05% CPU.       │
      │  │  Wakes when the network task sets state = PLAYING.           │
      │  └──────────────────────────────────────────────────────────────┘
      │
      │  ┌──────────────────────────────────────────────────────────────┐
      │  │  PHASE 2: Enable I2S                                        │
      │  │                                                              │
      │  │  i2s_channel_enable(s_tx_handle)                            │
      │  │    → BCLK and WS clocks start toggling on GPIO9/10          │
      │  │    → The amplifier sees clock edges, wakes up                │
      │  │    → s_i2s_enabled = true                                    │
      │  │                                                              │
      │  │  (skipped in SPEAKER_SIMULATE mode)                          │
      │  └──────────────────────────────────────────────────────────────┘
      │
      │  ┌──────────────────────────────────────────────────────────────┐
      │  │  PHASE 3: Playback loop                                     │
      │  │                                                              │
      │  │  while (state == PLAYING || state == DRAINING) {            │
      │  │                                                              │
      │  │      ┌──────────────────────────────────────┐                │
      │  │      │  3a. Read from stream buffer          │                │
      │  │      │                                      │                │
      │  │      │  received = xStreamBufferReceive(    │                │
      │  │      │      stream, play_buf, 1024, 50ms)   │                │
      │  │      │                                      │                │
      │  │      │  Blocks up to 50ms if buffer empty.  │                │
      │  │      └──────────┬───────────────────────────┘                │
      │  │                 │                                            │
      │  │          ┌──────┴──────┐                                     │
      │  │          │ received>0? │                                     │
      │  │          └──┬──────┬───┘                                     │
      │  │         YES │      │ NO                                      │
      │  │             │      │                                         │
      │  │             ▼      │                                         │
      │  │  ┌──────────────┐  │                                         │
      │  │  │ 3b. Volume   │  │                                         │
      │  │  │   scaling    │  │                                         │
      │  │  │              │  │                                         │
      │  │  │ for each     │  │                                         │
      │  │  │ int16 sample:│  │                                         │
      │  │  │   sample *=  │  │                                         │
      │  │  │   volume     │  │                                         │
      │  │  │ (0.0–1.0)    │  │                                         │
      │  │  └──────┬───────┘  │                                         │
      │  │         │          │                                         │
      │  │         ▼          │                                         │
      │  │  ┌──────────────┐  │                                         │
      │  │  │ 3c. I2S      │  │                                         │
      │  │  │   write      │  │                                         │
      │  │  │              │  │                                         │
      │  │  │ i2s_channel_ │  │                                         │
      │  │  │ write(buf,   │  │                                         │
      │  │  │   1000ms     │  │                                         │
      │  │  │   timeout)   │  │                                         │
      │  │  │              │  │                                         │
      │  │  │ Blocks until │  │                                         │
      │  │  │ DMA accepts  │  │                                         │
      │  │  └──────┬───────┘  │                                         │
      │  │         │          ▼                                         │
      │  │         │   ┌──────────────┐                                 │
      │  │         │   │ DRAINING?    │                                 │
      │  │         │   │   buffer     │                                 │
      │  │         │   │   empty?     │                                 │
      │  │         │   └──┬───────┬───┘                                 │
      │  │         │  YES │       │ NO (underrun)                       │
      │  │         │      │       │                                     │
      │  │         │      │       ▼                                     │
      │  │         │      │  ┌──────────────┐                           │
      │  │         │      │  │ Write silence│                           │
      │  │         │      │  │ (zeros) to   │                           │
      │  │         │      │  │ I2S to avoid │                           │
      │  │         │      │  │ clicks/pops  │                           │
      │  │         │      │  └──────┬───────┘                           │
      │  │         │      │         │                                   │
      │  │         │      ▼         │                                   │
      │  │         │  state=STOPPED │                                   │
      │  │         │  break         │                                   │
      │  │         │                │                                   │
      │  │         └──── loop ──────┘                                   │
      │  │                                                              │
      │  └──────────────────────────────────────────────────────────────┘
      │
      │  ┌──────────────────────────────────────────────────────────────┐
      │  │  PHASE 4: Disable I2S                                       │
      │  │                                                              │
      │  │  i2s_channel_disable(s_tx_handle)                           │
      │  │    → BCLK and WS clocks stop                                 │
      │  │    → DMA halted                                              │
      │  │    → Amplifier goes quiet                                    │
      │  │    → s_i2s_enabled = false                                   │
      │  │                                                              │
      │  │  if (state == STOPPED)                                       │
      │  │      state = IDLE   ◄── ready for next playback              │
      │  └──────────────────────────────────────────────────────────────┘
      │
      └── } // back to Phase 1 — wait for next speaker_play_url() call
```

---

### Step 4: Stop — `speaker_stop()`

Can be called from any task at any time. Immediately halts everything.

```
  speaker_stop()
  │
  │  1. state = IDLE
  │     → Network task checks this on next iteration and exits
  │     → Playback task's inner loop condition becomes false
  │
  │  2. vTaskDelete(s_network_task_handle)
  │     → Kills network task immediately if still running
  │     → HTTP client handle may leak (acceptable for forced stop)
  │
  │  3. vTaskDelay(100ms)
  │     → Brief pause for playback task to see IDLE and exit its loop
  │
  │  4. xStreamBufferReset(s_audio_stream)
  │     → Flush any remaining PCM data (not played)
  │     → Safe: no tasks are reading/writing the buffer now
  │
  │  5. Playback task disables I2S on its own (Phase 4)
  │
  └── State: IDLE, ready for next speaker_play_url()
```

---

## Parallel Task Timeline — Normal Playback

This diagram shows what both tasks are doing at each point in time during
a normal (non-interrupted) playback session.

```
  Time ──────────────────────────────────────────────────────────────────►

  CALLER TASK:
  ║ speaker_play_url()
  ║ │ state=BUFFERING
  ║ │ spawn net task
  ║ └ return
  ║ (caller continues      (caller can poll speaker_get_state()
  ║  its own work)          to know when playback finishes)

  NETWORK TASK:                                                  self-
  ║              ║ HTTP connect ║ read chunks into buffer ║ DRAIN ║ delete
  ║              ║──────────────║─────────────────────────║───────║──X
  ║              ║              ║ state=PLAYING           ║       ║
  ║              ║              ║ (prebuffer hit)         ║       ║

  PLAYBACK TASK:
  ║ sleeping     ║ sleeping     ║ enable I2S              ║ drain ║ disable
  ║ (poll 20ms)  ║ (poll 20ms)  ║ read→vol→I2S→repeat    ║ buf   ║ I2S
  ║──────────────║──────────────║─────────────────────────║───────║──────►
  ║              ║              ║                         ║ STOP  ║ IDLE
  ║              ║              ║                         ║ ↑     ║
  ║              ║              ║                         ║ buf   ║
  ║              ║              ║                         ║ empty ║

  STATE:
  ║    IDLE      ║  BUFFERING   ║       PLAYING           ║DRAIN  ║IDLE
  ║──────────────║──────────────║─────────────────────────║───────║──────►

  I2S HARDWARE:
  ║   disabled   ║   disabled   ║  enabled, clocks run    ║ run   ║disabled
  ║   (silent)   ║   (silent)   ║  PCM → amp → speaker    ║       ║(silent)
```

---

## Data Flow Through the Stream Buffer

The stream buffer is the core synchronization mechanism. It handles the
fundamental timing mismatch between network (bursty) and audio (steady).

```
  Network delivers data in bursts:
  ┌──────────────────────────────────────────────────────────────────┐
  │  ████████          ██████              ████████████       ████  │
  │  big chunk         pause               big chunk         small  │
  │  (fast)            (100ms)             (fast)             (end) │
  └──────────────────────────────────────────────────────────────────┘

  I2S consumes data at steady 32 KB/s:
  ┌──────────────────────────────────────────────────────────────────┐
  │  ████████████████████████████████████████████████████████████████│
  │  constant rate, never pauses                                    │
  └──────────────────────────────────────────────────────────────────┘

  Stream buffer absorbs the difference:
  ┌──────────────────────────────────────────────────────────────────┐
  │                                                                  │
  │  Fill level over time:                                          │
  │                                                                  │
  │  100% ┤                                                          │
  │       │                                                          │
  │   75% ┤         ╭──╮                 ╭────╮                      │
  │       │        ╱    ╲               ╱      ╲                     │
  │   50% ┤       ╱      ╲             ╱        ╲                    │
  │       │      ╱        ╲           ╱          ╲                   │
  │   25% ┤ ···╱ threshold ╲─────────╱            ╲──────            │
  │       │   ╱  (playback   ╲       ╱              ╲                │
  │    0% ┤──╱    starts)     ╲─────╱                ╲──── empty     │
  │       └──┬────┬────┬────┬────┬────┬────┬────┬────┬────           │
  │         0ms  500  1000 1500 2000 2500 3000 3500 4000             │
  │                                                                  │
  │  Back-pressure: when buffer reaches 100%, the network task's     │
  │  xStreamBufferSend() blocks until the playback task frees space. │
  │                                                                  │
  │  Underrun: when buffer reaches 0% during PLAYING, the playback   │
  │  task writes silence (zeros) to I2S to prevent clicks.           │
  └──────────────────────────────────────────────────────────────────┘
```

---

## Volume Scaling Path

Software volume scaling is applied by the playback task between the buffer
read and the I2S write. It's the only processing done on the PCM data.

```
  Raw PCM from stream buffer           After volume scaling (volume = 0.5)
  ┌──────────────────────┐             ┌──────────────────────┐
  │ Sample 0:  +16384    │  × 0.5 ──►  │ Sample 0:  +8192     │
  │ Sample 1:  +32767    │  × 0.5 ──►  │ Sample 1:  +16383    │
  │ Sample 2:  -10000    │  × 0.5 ──►  │ Sample 2:  -5000     │
  │ Sample 3:  +0        │  × 0.5 ──►  │ Sample 3:  +0        │
  │ Sample 4:  -32768    │  × 0.5 ──►  │ Sample 4:  -16384    │
  │ ...                  │             │ ...                  │
  └──────────────────────┘             └──────────────────────┘
                                            │
                                            ▼
                                       i2s_channel_write()

  Controlled by:
    speaker_set_volume(0.5f)  → s_speaker_cfg.volume = 0.5
    VOLUME_SCALING_ENABLED=0  → bypass entirely (no multiply)
```

---

## File Map

```
  main/
  ├── main.c                 2 lines added:  speaker_init() + xTaskCreate
  ├── speaker_driver.h       Public API:     6 functions + config struct + state enum
  ├── speaker_driver.c       Implementation: ~685 lines
  │   ├── Configuration      Audio preset, pin defines, buffer sizes, volume
  │   ├── apply_volume()     Software volume scaling (16-bit PCM × float)
  │   ├── speaker_init()     I2S peripheral + stream buffer setup
  │   ├── speaker_network_   Ephemeral HTTP producer task
  │   │   task()
  │   ├── speaker_get_state  Public API: query state
  │   ├── speaker_set_volume Public API: set volume [0.0, 1.0]
  │   ├── speaker_stop()     Public API: immediate stop
  │   ├── speaker_play_url() Public API: start playback from URL
  │   └── speaker_playback_  Permanent I2S consumer task
  │       task()
  ├── modem_driver.c/h       (existing — unmodified)
  ├── wifi_driver.c/h        (existing — unmodified)
  ├── network_app.c/h        (existing — unmodified)
  └── CMakeLists.txt         1 line changed: added "speaker_driver.c" to SRCS
```

---

## Configuration Quick Reference

All compile-time settings are `#define`s at the top of `speaker_driver.c`:

| Define | Default | What it controls |
|--------|---------|------------------|
| `AUDIO_PRESET` | `AUDIO_PRESET_SPEECH` | `SPEECH` = 16kHz/16-bit/mono, `MUSIC` = 44.1kHz/16-bit/stereo |
| `I2S_BCLK_PIN` | `9` (GPIO9, D10) | I2S bit clock GPIO |
| `I2S_WS_PIN` | `10` (GPIO10, D7) | I2S word select / LRCLK GPIO |
| `I2S_DOUT_PIN` | `44` (GPIO44, D6) | I2S data out GPIO |
| `RING_BUFFER_SIZE` | `16384` (16 KB) | Stream buffer size in bytes |
| `PREBUFFER_THRESHOLD` | `4096` (25%) | Bytes buffered before playback starts |
| `VOLUME_SCALING_ENABLED` | `1` | `1` = apply software volume, `0` = bypass |
| `DEFAULT_VOLUME` | `0.8f` | Initial volume (0.0 = mute, 1.0 = full) |
| `SPEAKER_SIMULATE` | `0` | `1` = log I2S writes instead of real hardware |
