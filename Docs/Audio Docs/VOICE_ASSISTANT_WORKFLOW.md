# Voice Assistant — Workflow

This document walks through the voice assistant's complete lifecycle: how
it initializes, how it orchestrates the mic and speaker drivers through a
push-to-talk conversation loop, and why having it as a separate FreeRTOS
task is essential for non-blocking audio I/O.

---

## Why the Voice Assistant Exists

The mic and speaker drivers are reusable hardware modules. They know _how_
to capture audio and _how_ to play audio, but they don't know _when_ or
_in what order_. Something must decide:

- When to start recording (button press)
- When to stop recording (button release)
- Where to send the recording (backend URL)
- When the upload is done
- Where to get the response audio (backend URL)
- When playback is done
- When to listen for the next button press

The voice assistant is that something. It's a pure application-layer
orchestrator — it owns no hardware, no buffers, no I2S handles. It calls
the public APIs of both drivers and polls their states.

```
  Without voice assistant:                With voice assistant:
  ────────────────────────                ──────────────────────

  Who calls mic_start()?                  voice_assistant_task calls mic_start()
  Who calls mic_stop()?                   voice_assistant_task calls mic_stop()
  Who calls mic_upload()?                 voice_assistant_task calls mic_upload()
  Who waits for upload?                   voice_assistant_task polls mic_get_state()
  Who calls speaker_play_url()?           voice_assistant_task calls speaker_play_url()
  Who waits for playback?                 voice_assistant_task polls speaker_get_state()
  Who handles the button?                 voice_assistant_task reads GPIO

  The drivers remain simple, focused, and testable in isolation.
  The voice assistant handles all sequencing and user interaction.
```

---

## High-Level Architecture

The voice assistant sits above both drivers as a thin orchestration layer.
It uses only public API functions — it never touches driver internals
(buffers, I2S handles, HTTP clients, task handles).

```
  ┌──────────────────────────────────────────────────────────────────────┐
  │                         APPLICATION LAYER                            │
  │                                                                      │
  │  ┌────────────────────────────────────────────────────────────────┐  │
  │  │                    voice_assistant_task                        │  │
  │  │                    (priority 5, permanent)                     │  │
  │  │                                                                │  │
  │  │  Owns: push-to-talk button GPIO                                │  │
  │  │  Does: button polling, driver sequencing, state polling        │  │
  │  │  Uses: mic driver API + speaker driver API + network_app API   │  │
  │  └────────────┬───────────────────────────────┬───────────────────┘  │
  │               │ public API calls only         │                      │
  └───────────────┼───────────────────────────────┼──────────────────────┘
                  │                               │
  ┌───────────────▼──────────────┐  ┌─────────────▼──────────────────┐
  │         DRIVER LAYER         │  │         DRIVER LAYER           │
  │                              │  │                                │
  │     mic_driver.c/.h          │  │     speaker_driver.c/.h        │
  │                              │  │                                │
  │  ┌────────────────────────┐  │  │  ┌───────────────────────────┐ │
  │  │ mic_capture_task       │  │  │  │ speaker_playback_task     │ │
  │  │ (permanent, pri 6)     │  │  │  │ (permanent, pri 6)        │ │
  │  │ I2S RX → gain → buf    │  │  │  │ buf → volume → I2S TX     │ │
  │  └────────────────────────┘  │  │  └───────────────────────────┘ │
  │                              │  │                                │
  │  ┌────────────────────────┐  │  │  ┌───────────────────────────┐ │
  │  │ mic_upload_task        │  │  │  │ speaker_network_task      │ │
  │  │ (ephemeral, pri 5)     │  │  │  │ (ephemeral, pri 5)        │ │
  │  │ buf → HTTP POST        │  │  │  │ HTTP resp → buf           │ │
  │  └────────────────────────┘  │  │  └───────────────────────────┘ │
  │                              │  │                                │
  │  Owns: I2S RX, stream buf,   │  │  Owns: I2S TX, stream buf,     │
  │        HTTP upload client    │  │        HTTP download client    │
  └──────────────────────────────┘  └────────────────────────────────┘
```

### Entry Points — What the Voice Assistant Calls

The voice assistant interacts with both drivers through exactly these
functions. No internal state, buffers, or handles are ever accessed.

```
  ┌──────────────────────────────────────────────────────────────────────┐
  │  VOICE ASSISTANT → MIC DRIVER                                        │
  │                                                                      │
  │  mic_start()         Start I2S RX, capture task fills buffer         │
  │                      Returns immediately (synchronous state change)  │
  │                      Sets state: RECORDING                           │
  │                                                                      │
  │  mic_stop()          Stop I2S RX, data stays in buffer               │
  │                      Returns after 50ms (waits for capture exit)     │
  │                      Sets state: IDLE (buffer preserved)             │
  │                                                                      │
  │  mic_upload(url)     Spawn upload task, POST buffer to backend       │
  │                      Returns immediately (non-blocking)              │
  │                      Sets state: UPLOADING                           │
  │                                                                      │
  │  mic_get_state()     Read current state (IDLE/RECORDING/UPLOADING/   │
  │                      DONE/ERROR). Used by voice_asst to poll.        │
  │                      Returns: mic_state_t                            │
  └──────────────────────────────────────────────────────────────────────┘

  ┌──────────────────────────────────────────────────────────────────────┐
  │  VOICE ASSISTANT → SPEAKER DRIVER                                    │
  │                                                                      │
  │  speaker_play_url(url)  Spawn network task, fetch PCM, play it       │
  │                         Returns immediately (non-blocking)           │
  │                         Sets state: BUFFERING                        │
  │                                                                      │
  │  speaker_get_state()    Read current state (IDLE/BUFFERING/PLAYING/  │
  │                         DRAINING/STOPPED/ERROR). Used to poll.       │
  │                         Returns: speaker_state_t                     │
  └──────────────────────────────────────────────────────────────────────┘

  ┌──────────────────────────────────────────────────────────────────────┐
  │  VOICE ASSISTANT → NETWORK APP                                       │
  │                                                                      │
  │  network_app_wait_for_connection()                                   │
  │                         Block until WiFi/PPP has an IP address.      │
  │                         Called once at task start. Ensures network   │
  │                         is ready before any HTTP operations.         │
  └──────────────────────────────────────────────────────────────────────┘
```

---

## Why a Separate Task — The Non-Blocking Problem

Without the voice assistant task, the conversation flow would need to live
inside `app_main()` or inside one of the drivers. Both options cause
blocking problems:

### Problem: Blocking HTTP in a Single Thread

```
  Naive approach — all in one place, single thread:

  while (1) {
      wait_for_button();
      mic_start();                          // OK — fast
      wait_for_button_release();
      mic_stop();                           // OK — fast

      http_post(mic_data);                  // ⚠ BLOCKS 1-30 seconds
                                            // CPU is stuck here
                                            // No other work can happen
                                            // Can't poll button
                                            // Can't run speaker
                                            // Can't respond to WiFi events

      http_get(response);                   // ⚠ BLOCKS 1-30 seconds again
      play_audio(response);                 // ⚠ BLOCKS until done
  }

  Total time stuck: could be 60+ seconds per conversation turn.
  During this time, the ESP32 can't do anything else.
```

### Solution: Voice Assistant Task + Driver Tasks

FreeRTOS solves this by splitting work across tasks. The voice assistant
task handles sequencing while driver tasks handle the blocking I/O:

```
  Multi-task approach — voice assistant orchestrates:

  voice_assistant_task:                driver tasks (separate):
  ──────────────────────               ────────────────────────

  mic_start()  ─────────────────────► capture task: I2S read loop
      │ returns immediately              (runs independently at pri 6)
      │
  poll button (20ms delay)              capture task: still reading...
  poll button (20ms delay)              capture task: still reading...
      │                                 (voice asst and capture
      │                                  run concurrently)
      │
  mic_stop()   ─────────────────────► capture task: exits loop
      │ returns after 50ms
      │
  mic_upload() ─────────────────────► upload task spawned: HTTP POST
      │ returns immediately              (runs independently at pri 5)
      │
  poll mic_get_state() (100ms)          upload task: sending data...
  poll mic_get_state() (100ms)          upload task: sending data...
  poll mic_get_state() (100ms)          upload task: done → IDLE
      │ sees IDLE → continue                 (self-deletes)
      │
  speaker_play_url() ──────────────► net task spawned: HTTP recv
      │ returns immediately              (runs independently at pri 5)
      │                                 playback task: buffer → I2S
  poll speaker_get_state() (200ms)       (runs independently at pri 6)
  poll speaker_get_state() (200ms)
      │ sees IDLE → loop
      │
  back to button polling

  Total blocking time for voice_asst: 0 seconds.
  All HTTP I/O runs in separate tasks.
  All I2S I/O runs in separate tasks.
  Voice assistant is always responsive.
```

### The CPU Budget During a Conversation Turn

```
  ┌─────────────────────────────────────────────────────────────────────┐
  │  What each task does with the CPU during one conversation:          │
  │                                                                     │
  │  voice_assistant_task (priority 5):                                 │
  │    button poll:  vTaskDelay(20ms) — sleeping 99.95% of the time     │
  │    state poll:   vTaskDelay(100-200ms) — sleeping 99.9% of time     │
  │    API calls:    mic_start/stop/upload, speaker_play_url — <1ms     │
  │    Total CPU:    <0.1% of one core                                  │
  │                                                                     │
  │  mic_capture_task (priority 6):                                     │
  │    I2S read:     blocks on DMA (0% CPU while waiting for samples)   │
  │    gain calc:    ~1% CPU at 16 kHz (simple multiply + clamp)        │
  │    buffer write: xStreamBufferSend — near instant                   │
  │    Total CPU:    ~1% during recording, 0% otherwise                 │
  │                                                                     │
  │  mic_upload_task (priority 5, ephemeral):                           │
  │    HTTP write:   blocks on TCP send (0% CPU while kernel sends)     │
  │    buffer read:  xStreamBufferReceive — near instant                │
  │    Total CPU:    ~1% during upload, doesn't exist otherwise         │
  │                                                                     │
  │  speaker_network_task (priority 5, ephemeral):                      │
  │    HTTP recv:    blocks on TCP recv (0% CPU while waiting)          │
  │    buffer write: xStreamBufferSend — near instant                   │
  │    Total CPU:    ~1% during download, doesn't exist otherwise       │
  │                                                                     │
  │  speaker_playback_task (priority 6):                                │
  │    buffer read:  blocks up to 50ms if empty (0% CPU)                │
  │    volume calc:  ~1% CPU at 16 kHz                                  │
  │    I2S write:    blocks on DMA (0% CPU while DMA transfers)         │
  │    Total CPU:    ~1% during playback, 0% otherwise                  │
  │                                                                     │
  │  Combined total during peak (recording): ~3% of one core            │
  │  Combined total during peak (playback):  ~3% of one core            │
  │  The ESP32-S3 has two cores — audio uses <2% of total capacity.     │
  └─────────────────────────────────────────────────────────────────────┘
```

---

## Workflow Step by Step

### Step 0: Boot — Initialization

The voice assistant is created in `app_main()` after both drivers are
initialized. This ordering ensures mic and speaker hardware is ready before
the voice assistant tries to use them.

```
  app_main()
  │
  ├── 1. System init (netif, event loop, network, WiFi)
  │
  ├── 2. speaker_init()                 ◄── I2S TX + buffer allocated
  │      xTaskCreate(speaker_playback_task, pri 6)
  │
  ├── 3. mic_init()                     ◄── I2S RX + buffer allocated
  │      xTaskCreate(mic_capture_task, pri 6)
  │
  ├── 4. voice_assistant_init()         ◄── button GPIO configured
  │      xTaskCreate(voice_assistant_task, pri 5)
  │
  └── return (FreeRTOS scheduler takes over)
```

**What `voice_assistant_init()` does:**

```
  voice_assistant_init()
  │
  │  ┌──────────────────────────────────────────────────────────────┐
  │  │ BUTTON GPIO SETUP (only if PTT_BUTTON_ENABLED && MIC_ENABLED)│
  │  │                                                              │
  │  │  gpio_config_t io_conf = {                                   │
  │  │      .pin_bit_mask  = (1ULL << GPIO4),  // D3 on XIAO        │
  │  │      .mode          = GPIO_MODE_INPUT,                       │
  │  │      .pull_up_en    = GPIO_PULLUP_ENABLE,                    │
  │  │      .pull_down_en  = GPIO_PULLDOWN_DISABLE,                 │
  │  │      .intr_type     = GPIO_INTR_DISABLE,                     │
  │  │  };                                                          │
  │  │  gpio_config(&io_conf);                                      │
  │  │                                                              │
  │  │  Button wiring:                                              │
  │  │                                                              │
  │  │    GPIO4 ──── internal pull-up (HIGH when idle)              │
  │  │         │                                                    │
  │  │     ┌───┴───┐                                                │
  │  │     │ Button│  momentary switch (normally open)              │
  │  │     └───┬───┘                                                │
  │  │         │                                                    │
  │  │       GND ── pressed = LOW (0), released = HIGH (1)          │
  │  └──────────────────────────────────────────────────────────────┘
  │
  └── Log: "Voice assistant initialized (MIC_ENABLED=1, PTT_BUTTON=1)"
```

After boot, the system is in this state:

```
  Mic driver:      IDLE (I2S RX configured, disabled, buffer empty)
  Speaker driver:  IDLE (I2S TX configured, disabled, buffer empty)
  Capture task:    sleeping (polling for RECORDING state)
  Playback task:   sleeping (polling for PLAYING state)
  Voice assistant: about to start — will block on network first
  Button GPIO:     configured, reading HIGH (not pressed)
```

---

### Step 1: Wait for Network

The first thing the voice assistant task does is block until WiFi (or
cellular PPP) has acquired an IP address. No audio operations can work
without network connectivity.

```
  voice_assistant_task()
  │
  ├── network_app_wait_for_connection()
  │     │
  │     │  Blocks on an EventGroup bit set by the WiFi event handler.
  │     │  0% CPU while waiting (task is suspended by the scheduler).
  │     │
  │     │  When WiFi connects and DHCP assigns an IP:
  │     │    → WiFi event handler sets IP_READY_BIT
  │     │    → xEventGroupWaitBits() returns
  │     │    → voice_assistant_task resumes
  │     │
  │     └── Returns (network is now ready)
  │
  └── ESP_LOGI: "Network ready — voice assistant active"
```

This is critical: without this gate, `mic_upload()` and `speaker_play_url()`
would attempt HTTP connections before WiFi is established, causing immediate
failures.

```
  Boot timeline — chronological:

  1. WiFi driver starts scanning for AP
  2. WiFi driver connects, begins DHCP
  3. voice_assistant_task calls network_app_wait_for_connection()
       → task is BLOCKED (suspended by scheduler, 0% CPU)
       → mic/speaker tasks are sleeping in IDLE (polling for commands)
  4. DHCP completes → IP acquired → IP_READY_BIT set
  5. network_app_wait_for_connection() returns
  6. voice_assistant_task resumes → enters button polling loop
```

---

### Step 2: Button Press — Start Recording

The voice assistant polls the button GPIO every 20ms. When the button is
pressed (GPIO reads LOW), it calls `mic_start()`.

```
  voice_assistant_task (in main loop)
  │
  │  ┌──────────────────────────────────────────────────────────────┐
  │  │  BUTTON POLLING LOOP                                         │
  │  │                                                              │
  │  │  while (!button_pressed()) {                                 │
  │  │      vTaskDelay(pdMS_TO_TICKS(20));                          │
  │  │  }                                                           │
  │  │                                                              │
  │  │  How this works:                                             │
  │  │                                                              │
  │  │    button_pressed() returns:                                 │
  │  │      gpio_get_level(GPIO4) == 0                              │
  │  │                                                              │
  │  │    Not pressed: gpio=1 → false → sleep 20ms → check again    │
  │  │    Pressed:     gpio=0 → true  → exit loop                   │
  │  │                                                              │
  │  │  The 20ms interval provides natural debouncing:              │
  │  │                                                              │
  │  │    Real button press:                                        │
  │  │    ───HIGH──╲                                                │
  │  │              ╲ bounce  ╲                                     │
  │  │    ───LOW─────╱─────────╲──── stable LOW ────                │
  │  │              ▲           ▲                                   │
  │  │            poll 1      poll 2                                │
  │  │            (sees HIGH, (sees LOW,                            │
  │  │             keeps      exits loop)                           │
  │  │             waiting)                                         │
  │  │                                                              │
  │  │  CPU cost: 0.05% (one gpio_get_level call every 20ms)        │
  │  └──────────────────────────────────────────────────────────────┘
  │
  │  Button pressed detected!
  │
  └── mic_start()
      │
      │  What happens inside mic_start():
      │
      │  ┌─── voice_assistant_task (this thread) ─────────────────┐
      │  │  1. Reset buffer: xStreamBufferReset()                 │
      │  │  2. Reset counter: s_captured_bytes = 0                │
      │  │  3. Set state: MIC_STATE_RECORDING                     │
      │  │  4. Return                                             │
      │  └────────────────────────────────────────────────────────┘
      │
      │  What happens in mic_capture_task (separate task, pri 6):
      │
      │  ┌─── mic_capture_task (wakes up within 20ms) ────────────┐
      │  │  1. Sees state == RECORDING                            │
      │  │  2. i2s_channel_enable(rx_handle)                      │
      │  │  3. Enters capture loop:                               │
      │  │       i2s_channel_read() → apply_gain() → buffer_send()│
      │  │  4. Loops until state != RECORDING                     │
      │  └────────────────────────────────────────────────────────┘
      │
      │  Both tasks run concurrently from this point.
```

---

### Step 3: Button Held — Recording in Progress

While the user holds the button, the voice assistant does nothing but
poll for button release. Meanwhile, the mic capture task runs independently
at higher priority, reading I2S and filling the buffer.

```
  Concurrent activity while button is held:

  voice_assistant_task (pri 5):       mic_capture_task (pri 6):
  ──────────────────────────────      ──────────────────────────────

  while (button_pressed()) {          while (state == RECORDING) {
      vTaskDelay(20ms);                   i2s_channel_read(buf, 1024);
  }                                       apply_gain(buf, gain);
                                          xStreamBufferSend(buf);
  (sleeping 99.95% of the time)           s_captured_bytes += sent;
  (just checking GPIO every 20ms)     }
                                      (running at ~1% CPU, blocks
                                       on I2S DMA the rest of time)


  These two tasks never compete:
    - voice_asst sleeps in vTaskDelay (yields CPU)
    - capture task blocks on i2s_channel_read (yields CPU)
    - When DMA has data, capture task wakes at pri 6 (preempts)
    - When 20ms expires, voice_asst wakes at pri 5 (runs if CPU free)
```

---

### Step 4: Button Release — Stop Recording

When the button is released (GPIO reads HIGH), the voice assistant calls
`mic_stop()`. This changes the state to IDLE. The capture task sees the
change on its next loop iteration and disables I2S.

```
  voice_assistant_task
  │
  ├── button_pressed() returns false → exit polling loop
  │
  └── mic_stop()
      │
      │  What happens across tasks:
      │
      │  voice_assistant_task:             mic_capture_task:
      │  ─────────────────────             ──────────────────
      │
      │  mic_stop():                       (still in capture loop)
      │    state = MIC_STATE_IDLE ─────►   loop check: state != RECORDING
      │    vTaskDelay(50ms)                  → exit inner loop
      │      (waits for capture              → i2s_channel_disable()
      │       task to react)                 → s_i2s_enabled = false
      │    return                            → back to Phase 1 (sleep)
      │
      │  After mic_stop() returns:
      │
      │  ┌──────────────────────────────────────────────────────────┐
      │  │  Mic state:     IDLE                                     │
      │  │  I2S RX:        DISABLED (clocks stopped, mic idle)      │
      │  │  Stream buffer: contains N bytes of captured PCM         │
      │  │  Byte counter:  s_captured_bytes = N                     │
      │  │  Capture task:  sleeping in Phase 1 poll                 │
      │  │                                                          │
      │  │  IMPORTANT: buffer is NOT flushed.                       │
      │  │  The captured data is preserved for mic_upload().        │
      │  └──────────────────────────────────────────────────────────┘
```

---

### Step 5: Upload Audio to Backend

The voice assistant calls `mic_upload(url)` which spawns an ephemeral
upload task. The call returns immediately — the voice assistant then
polls `mic_get_state()` until the upload completes.

```
  voice_assistant_task
  │
  ├── mic_upload("http://backend.example.com/api/listen")
  │     │
  │     │  Inside mic_upload():
  │     │    1. Copy URL to static buffer (caller's string may be const)
  │     │    2. state = MIC_STATE_UPLOADING
  │     │    3. xTaskCreate(mic_upload_task, pri 5) ─── spawns new task
  │     │    4. return immediately
  │     │
  │     └── Returns (non-blocking)
  │
  │  Now three things exist simultaneously:
  │
  │  ┌─────────────────────────────────────────────────────────────────┐
  │  │  voice_assistant_task:  polling mic_get_state() every 100ms     │
  │  │  mic_upload_task:       reading buffer, writing HTTP body       │
  │  │  mic_capture_task:      sleeping (state is not RECORDING)       │
  │  │  speaker_playback_task: sleeping (state is not PLAYING)         │
  │  └─────────────────────────────────────────────────────────────────┘
  │
  │  ┌──────────────────────────────────────────────────────────────┐
  │  │  STATE POLLING LOOP                                          │
  │  │                                                              │
  │  │  while (1) {                                                 │
  │  │      mic_state_t s = mic_get_state();                        │
  │  │                                                              │
  │  │      if (s == MIC_STATE_IDLE || s == MIC_STATE_DONE)         │
  │  │          break;             // upload succeeded              │
  │  │                                                              │
  │  │      if (s == MIC_STATE_ERROR)                               │
  │  │          break;             // upload failed                 │
  │  │                                                              │
  │  │      vTaskDelay(100ms);     // sleep, let upload task run    │
  │  │  }                                                           │
  │  │                                                              │
  │  │  Why 100ms polling interval:                                 │
  │  │    - Upload of 16 KB takes ~200-500ms on WiFi                │
  │  │    - 100ms gives fast response without excessive polling     │
  │  │    - CPU cost: one enum read every 100ms ≈ 0.001%            │
  │  └──────────────────────────────────────────────────────────────┘
  │
  │  Meanwhile, the upload task does the actual work:
  │
  │  ┌──────────────────────────────────────────────────────────────┐
  │  │  mic_upload_task (ephemeral, priority 5):                    │
  │  │                                                              │
  │  │  1. esp_http_client_init(url, POST, 30s timeout)             │
  │  │  2. Set headers:                                             │
  │  │       Content-Type: application/octet-stream                 │
  │  │       X-Audio-Format: pcm;rate=16000;bits=16;channels=1      │
  │  │  3. esp_http_client_open(content_length = N)                 │
  │  │                                                              │
  │  │  4. while (bytes_remaining > 0):                             │
  │  │       xStreamBufferReceive(buf, 1024, 1s timeout)            │
  │  │       esp_http_client_write(buf, received)  ← TCP send       │
  │  │       (blocks until kernel accepts data — 0% CPU)            │
  │  │                                                              │
  │  │  5. esp_http_client_fetch_headers() → check HTTP 200         │
  │  │  6. state = DONE → brief delay → state = IDLE                │
  │  │  7. Cleanup, self-delete                                     │
  │  └──────────────────────────────────────────────────────────────┘
  │
  │  Voice assistant sees IDLE or DONE → continues
  │
  ├── if (MIC_STATE_ERROR) → log, delay 1s, continue (skip playback)
  │
  └── Proceed to Step 6
```

**Why the voice assistant doesn't do the upload itself:**

```
  If voice_assistant_task did the HTTP POST directly:

    mic_stop();
    http_post(data);     ← BLOCKS for 200ms–30s
                         ← Voice assistant can't respond to anything
                         ← Can't abort, can't timeout gracefully
                         ← If backend is slow: entire system stuck

  With the upload in a separate task:

    mic_stop();
    mic_upload(url);     ← returns in <1ms
    poll state;          ← voice assistant stays responsive
                         ← Could add timeout logic
                         ← Could add abort-on-button-press
                         ← System remains responsive
```

---

### Step 6: Play Backend Response

After the upload completes, the voice assistant calls `speaker_play_url()`
to play the backend's audio response. This follows the same non-blocking
pattern: call returns immediately, poll for completion.

```
  voice_assistant_task
  │
  ├── speaker_play_url("http://backend.example.com/api/speak")
  │     │
  │     │  Inside speaker_play_url():
  │     │    1. If already playing → speaker_stop() first
  │     │    2. Copy URL to static buffer
  │     │    3. xStreamBufferReset() (clear speaker buffer)
  │     │    4. state = SPEAKER_STATE_BUFFERING
  │     │    5. xTaskCreate(speaker_network_task, pri 5) ─ spawns
  │     │    6. return immediately
  │     │
  │     └── Returns (non-blocking)
  │
  │  Now the speaker pipeline is active:
  │
  │  ┌─────────────────────────────────────────────────────────────────┐
  │  │  speaker_network_task (ephemeral, pri 5):                       │
  │  │    HTTP POST to backend                                         │
  │  │    recv() BLOCKS until backend sends first PCM chunk            │
  │  │    (backend is processing: STT → LLM → TTS → transcode)         │
  │  │    This may take 1–10 seconds. Task sleeps at 0% CPU.           │
  │  │    When chunks arrive: writes to stream buffer                  │
  │  │    When pre-buffer threshold hit: state = PLAYING               │
  │  │    When HTTP ends: state = DRAINING, self-deletes               │
  │  │                                                                 │
  │  │  speaker_playback_task (permanent, pri 6):                      │
  │  │    Polling for PLAYING state (20ms interval)                    │
  │  │    When PLAYING: enable I2S TX                                  │
  │  │    Read buffer → apply volume → i2s_channel_write()             │
  │  │    When DRAINING + buffer empty: STOPPED → IDLE                 │
  │  └─────────────────────────────────────────────────────────────────┘
  │
  │  ┌──────────────────────────────────────────────────────────────┐
  │  │  STATE POLLING LOOP                                          │
  │  │                                                              │
  │  │  while (1) {                                                 │
  │  │      speaker_state_t s = speaker_get_state();                │
  │  │                                                              │
  │  │      if (s == SPEAKER_STATE_IDLE)                            │
  │  │          break;             // playback complete             │
  │  │                                                              │
  │  │      if (s == SPEAKER_STATE_ERROR)                           │
  │  │          break;             // playback failed               │
  │  │                                                              │
  │  │      vTaskDelay(200ms);     // sleep, let audio tasks run    │
  │  │  }                                                           │
  │  │                                                              │
  │  │  Why 200ms polling interval (vs 100ms for mic):              │
  │  │    - Playback typically lasts 2-30 seconds                   │
  │  │    - Coarser polling is fine — user can hear when it stops   │
  │  │    - 200ms still detects completion within one poll cycle    │
  │  └──────────────────────────────────────────────────────────────┘
  │
  └── Sees IDLE → "Playback complete"
```

---

### Step 7: Loop — Next Conversation Turn

After playback finishes, the voice assistant pauses briefly (200ms) and
returns to Step 2, waiting for the next button press. The entire cycle
repeats.

```
  Complete state sequence of one conversation turn:

  ┌────────────────┐    ┌────────────────┐    ┌────────────────┐
  │   MIC STATE    │    │ SPEAKER STATE  │    │ VOICE ASST     │
  ├────────────────┤    ├────────────────┤    │ ACTIVITY       │
  │                │    │                │    ├────────────────┤
  │  IDLE          │    │  IDLE          │    │ button poll    │ Step 2
  │  ──────────    │    │  ──────────    │    │ (20ms loop)    │
  │                │    │                │    │                │
  │  RECORDING     │    │  IDLE          │    │ button poll    │ Step 3
  │  ──────────    │    │  ──────────    │    │ (20ms loop)    │
  │                │    │                │    │                │
  │  IDLE          │    │  IDLE          │    │ mic_stop()     │ Step 4
  │  (data in buf) │    │                │    │                │
  │                │    │                │    │                │
  │  UPLOADING     │    │  IDLE          │    │ state poll     │ Step 5
  │  ──────────    │    │  ──────────    │    │ (100ms loop)   │
  │  DONE → IDLE   │    │                │    │                │
  │                │    │                │    │                │
  │  IDLE          │    │  BUFFERING     │    │ state poll     │ Step 6
  │                │    │  ──────────    │    │ (200ms loop)   │
  │                │    │  PLAYING       │    │                │
  │                │    │  ──────────    │    │                │
  │                │    │  DRAINING      │    │                │
  │                │    │  ──────────    │    │                │
  │                │    │  STOPPED→IDLE  │    │                │
  │                │    │                │    │                │
  │  IDLE          │    │  IDLE          │    │ delay 200ms    │ Step 7
  │                │    │                │    │ → loop Step 2  │
  └────────────────┘    └────────────────┘    └────────────────┘

  Key observation: mic and speaker are never active simultaneously.
  The voice assistant enforces strict sequencing:
    record → upload → play → repeat (never overlapping)
```

---

## Error Handling

The voice assistant handles errors from both drivers without crashing.
Each error is logged and the conversation continues.

```
  Error recovery flow:

  ┌──────────────────────────────────────────────────────────────────────┐
  │  MIC UPLOAD ERROR                                                    │
  │                                                                      │
  │  mic_upload() fails (HTTP timeout, connection refused, etc.)         │
  │    → upload task sets state = MIC_STATE_ERROR                        │
  │    → voice_asst poll loop sees ERROR                                 │
  │    → logs: "Upload error — skipping playback"                        │
  │    → skips speaker_play_url() (no point playing without upload)      │
  │    → delays 1 second (avoid rapid retry)                             │
  │    → continues to top of loop (wait for next button press)           │
  │                                                                      │
  │  The next mic_start() call resets the mic state to RECORDING,        │
  │  which clears the error. No manual reset needed.                     │
  └──────────────────────────────────────────────────────────────────────┘

  ┌──────────────────────────────────────────────────────────────────────┐
  │  SPEAKER PLAYBACK ERROR                                              │
  │                                                                      │
  │  speaker_play_url() fails (HTTP error, I2S error, etc.)              │
  │    → network/playback task sets state = SPEAKER_STATE_ERROR          │
  │    → voice_asst poll loop sees ERROR                                 │
  │    → logs: "Playback error — check backend and network"              │
  │    → continues to top of loop (wait for next button press)           │
  │                                                                      │
  │  The next speaker_play_url() call resets state and stream buffer.    │
  └──────────────────────────────────────────────────────────────────────┘

  ┌──────────────────────────────────────────────────────────────────────┐
  │  NETWORK NOT READY                                                   │
  │                                                                      │
  │  network_app_wait_for_connection() blocks indefinitely until         │
  │  WiFi/PPP has an IP. If the connection drops later:                  │
  │    → mic_upload() will fail with HTTP error → handled above          │
  │    → speaker_play_url() will fail → handled above                    │
  │                                                                      │
  │  The voice assistant does not monitor network state continuously.    │
  │  It relies on HTTP call failures to detect connectivity issues.      │
  └──────────────────────────────────────────────────────────────────────┘
```

---

## Auto-Record Mode — Testing Without a Button

When `PTT_BUTTON_ENABLED=0`, the voice assistant replaces button polling
with a fixed-duration timer. This enables testing on Wokwi or on a bench
without a physical button.

```
  PTT_BUTTON_ENABLED=1 (default):       PTT_BUTTON_ENABLED=0:
  ──────────────────────────────         ─────────────────────

  Step 2: Wait for button press          Step 2: Delay 1 second
  while (!button_pressed())              vTaskDelay(1000ms)
      vTaskDelay(20ms);                  (auto-start after 1s pause)

  Step 3: Wait for button release        Step 3: Record for N seconds
  while (button_pressed())              vTaskDelay(PTT_AUTO_RECORD_MS)
      vTaskDelay(20ms);                  (default: 3000ms = 3 seconds)

  Steps 4–7: identical                   Steps 4–7: identical

  No GPIO configuration needed.          GPIO include and gpio_config()
  No button wiring needed.               are skipped at compile time.
```

---

## MIC_ENABLED Toggle — Speaker-Only Mode

When `MIC_ENABLED=0`, the voice assistant reverts to the original
speaker-only behavior: it calls `speaker_play_url()` in a loop without
any mic recording or button handling.

```
  MIC_ENABLED=1 (default):              MIC_ENABLED=0:
  ─────────────────────────              ──────────────

  1. Wait for button press               1. speaker_play_url(URL)
  2. mic_start()                         2. Poll speaker_get_state()
  3. Wait for button release             3. Wait for IDLE/ERROR
  4. mic_stop()                          4. Loop to step 1
  5. mic_upload(url)
  6. Wait for upload IDLE                No mic, no button, no upload.
  7. speaker_play_url(url)               Useful for testing the speaker
  8. Wait for playback IDLE              driver in isolation.
  9. Loop to step 1

  Both modes use the same includes.      mic_driver.h is included but
  Button GPIO code is conditionally      mic functions are never called.
  compiled out when MIC_ENABLED=0.
```

---

## End-to-End Timeline — Full Conversation with Backend

This lists every event across all components during one complete
conversation turn, including the backend's processing time.

```
  ── t0: IDLE ─────────────────────────────────────────────────────────

  voice_assistant_task:  polling button every 20ms
  mic_capture_task:      sleeping (IDLE)
  speaker_playback_task: sleeping (IDLE)
  mic state:    IDLE        speaker state: IDLE
  I2S RX: off               I2S TX: off

  ── t1: USER PRESSES BUTTON ─────────────────────────────────────────

  voice_assistant_task:  detects press → calls mic_start()
  mic_start():           resets buffer, state → RECORDING

  ── t2: RECORDING ────────────────────────────────────────────────────

  mic_capture_task:      wakes, enables I2S RX, enters capture loop
                         (i2s_channel_read → apply_gain → xStreamBufferSend)
  voice_assistant_task:  polling button release every 20ms
  mic state:    RECORDING   speaker state: IDLE
  I2S RX: on                I2S TX: off

  ── t3: USER RELEASES BUTTON ────────────────────────────────────────

  voice_assistant_task:  detects release → calls mic_stop()
  mic_stop():            state → IDLE, waits 50ms for capture task
  mic_capture_task:      exits loop, disables I2S RX, returns to sleep
  voice_assistant_task:  calls mic_upload(BACKEND_MIC_URL)
  mic_upload():          state → UPLOADING, spawns mic_upload_task (pri 5)
  mic state:    UPLOADING   speaker state: IDLE
  I2S RX: off               I2S TX: off

  ── t4: UPLOAD IN PROGRESS ──────────────────────────────────────────

  mic_upload_task:       HTTP POST → writes buffer to request body over TCP
                         (blocks on TCP send — 0% CPU while kernel sends)
  voice_assistant_task:  polling mic_get_state() every 100ms

  ── t5: UPLOAD COMPLETE → BACKEND PROCESSING ────────────────────────

  mic_upload_task:       all bytes sent, receives HTTP 200 OK
  mic_upload_task:       state → DONE → IDLE, self-deletes
  voice_assistant_task:  sees IDLE → calls speaker_play_url(BACKEND_SPEAKER_URL)
  speaker_play_url():    state → BUFFERING, spawns speaker_network_task (pri 5)
  backend:               receives PCM audio, begins processing:
                           STT (speech-to-text) → LLM (generate response)
                           → TTS (text-to-speech) → transcode to PCM
  mic state:    IDLE        speaker state: BUFFERING
  I2S RX: off               I2S TX: off

  ── t6: BACKEND STARTS STREAMING RESPONSE ────────────────────────────

  speaker_network_task:  recv() was BLOCKED (0% CPU) while backend processed
                         backend starts sending PCM chunks
                         xStreamBufferSend() fills speaker buffer
                         buffer ≥ prebuffer threshold → state → PLAYING
  speaker_playback_task: wakes, enables I2S TX, enters playback loop
                         (xStreamBufferReceive → apply_volume → i2s_channel_write)
  voice_assistant_task:  polling speaker_get_state() every 200ms
  mic state:    IDLE        speaker state: PLAYING
  I2S RX: off               I2S TX: on (PCM → amp → speaker)

  ── t7: HTTP TRANSFER COMPLETE ───────────────────────────────────────

  speaker_network_task:  HTTP response ends, state → DRAINING, self-deletes
  speaker_playback_task: continues draining remaining buffer to I2S
  mic state:    IDLE        speaker state: DRAINING
  I2S RX: off               I2S TX: on

  ── t8: PLAYBACK COMPLETE ────────────────────────────────────────────

  speaker_playback_task: buffer empty → state → STOPPED → IDLE
                         disables I2S TX, returns to sleep
  voice_assistant_task:  sees IDLE → delays 200ms → loops to t0
  mic state:    IDLE        speaker state: IDLE
  I2S RX: off               I2S TX: off
```

---

## File Map

```
  main/
  ├── voice_assistant.h       Public API: 2 functions (init + task)
  ├── voice_assistant.c       Implementation: ~310 lines
  │   ├── MIC_ENABLED          Toggle: push-to-talk vs speaker-only
  │   ├── PTT_BUTTON_ENABLED   Toggle: physical button vs auto-record
  │   ├── PTT_BUTTON_PIN       GPIO4 (D3) — configurable
  │   ├── BACKEND_MIC_URL      Upload endpoint for mic audio
  │   ├── BACKEND_SPEAKER_URL  Playback endpoint for response audio
  │   ├── button_pressed()     GPIO read helper (active low)
  │   ├── voice_assistant_init GPIO config for button
  │   └── voice_assistant_task Main loop: button→mic→upload→speak→loop
  ├── mic_driver.h             Used: mic_start/stop/upload/get_state
  ├── speaker_driver.h         Used: speaker_play_url/get_state
  ├── network_app.h            Used: network_app_wait_for_connection
  └── main.c                   Creates voice_assistant_task at pri 5
```

---

## Configuration Quick Reference

All compile-time settings in `voice_assistant.c`:

| Define | Default | What it controls |
|--------|---------|------------------|
| `MIC_ENABLED` | `1` | `1` = push-to-talk loop, `0` = speaker-only (no mic) |
| `PTT_BUTTON_ENABLED` | `1` | `1` = physical GPIO button, `0` = auto-record timer |
| `PTT_BUTTON_PIN` | `4` (GPIO4, D3) | Push-to-talk button GPIO pin |
| `PTT_AUTO_RECORD_MS` | `3000` | Auto-record duration when button disabled (ms) |
| `BACKEND_MIC_URL` | `http://...` | Backend endpoint for mic audio upload (POST) |
| `BACKEND_SPEAKER_URL` | `http://...` | Backend endpoint for speaker audio (POST→stream) |
