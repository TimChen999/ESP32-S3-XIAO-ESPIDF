# Backend Integration — Connecting the ESP32 to Your Python Script

This document explains how to connect the ESP32 voice assistant firmware to
an external Python backend that handles STT → LLM → TTS processing. It covers
both Wokwi simulation (audio bridge + backend on the same PC) and real
hardware (ESP32 with I2S mic/speaker connecting to a backend over Wi-Fi).

---

## Overview

The ESP32 firmware already implements the complete device-side pipeline:

```
  Button press → mic records → upload audio → play response → repeat
```

Your Python backend already implements the complete processing pipeline:

```
  Receive audio → STT → LLM → TTS → return audio
```

Integration means connecting these two pipelines over HTTP. The ESP32
sends recorded mic audio to the backend and receives TTS audio back.
No firmware changes are needed — only two URLs need to be set, and the
backend needs to expose the right HTTP endpoint.

---

## How the ESP32 Talks to the Backend

The voice assistant task (`voice_assistant.c`) makes **two sequential HTTP
requests** per conversation turn. This is baked into the firmware:

```
  voice_assistant_task
  │
  │  Step 1: Record mic audio (push-to-talk)
  │
  │  Step 2: Upload mic audio to backend
  │          mic_upload(BACKEND_MIC_URL)
  │            └── HTTP POST to BACKEND_MIC_URL
  │                Body:         raw PCM bytes (16-bit signed, 16 kHz, mono)
  │                Content-Type: application/octet-stream
  │                X-Audio-Format: pcm;rate=16000;bits=16;channels=1
  │                Expects:      HTTP 200 OK
  │
  │  Step 3: Wait for upload to complete
  │
  │  Step 4: Request audio response from backend
  │          speaker_play_url(BACKEND_SPEAKER_URL)
  │            └── HTTP POST to BACKEND_SPEAKER_URL
  │                Body:         empty (content-length: 0)
  │                Content-Type: application/json
  │                X-Audio-Format: pcm;rate=16000;bits=16;channels=1
  │                Expects:      HTTP 200, body = raw PCM audio stream
  │
  │  Step 5: Stream PCM response through speaker
  │
  │  Step 6: Wait for playback to finish → loop back to Step 1
```

The two URLs are defined in `main/board_config.h` (centralised with all
other board-specific settings):

```c
#define BACKEND_MIC_URL         "http://host.wokwi.internal:5000/api/conversation"
#define BACKEND_SPEAKER_URL     "http://host.wokwi.internal:5000/api/conversation"
```

---

## Single-Endpoint Backend (Option A)

Both `BACKEND_MIC_URL` and `BACKEND_SPEAKER_URL` can point to the **same
URL**. The backend distinguishes between the two requests by checking
whether the request body contains audio data:

- **Request has a body** (Content-Type: application/octet-stream) → mic upload
- **Request has no body** (Content-Type: application/json, length 0) → speaker fetch

This means your backend exposes a single `/api/conversation` endpoint
that handles both operations.

### Request Flow Diagram

```
  ESP32                                    Backend (:5000)
  ─────                                    ────────────────

  ┌─────────────────────┐
  │ mic_upload()        │
  │                     │
  │ POST /api/convo     │──── body: [PCM bytes] ────────►  1. Receive PCM
  │ Content-Type:       │                                   2. STT → text
  │  octet-stream       │                                   3. LLM → response
  │ X-Audio-Format:     │                                   4. TTS → audio
  │  pcm;rate=16000;    │                                   5. Transcode to
  │  bits=16;channels=1 │                                      raw PCM s16le
  │                     │◄────────── HTTP 200 OK ────────   6. Store PCM
  └─────────────────────┘                                      in memory

  ┌─────────────────────┐
  │ speaker_play_url()  │
  │                     │
  │ POST /api/convo     │──── body: empty ───────────────►  7. Retrieve
  │ Content-Type:       │                                      stored PCM
  │  application/json   │
  │                     │◄──── body: [PCM bytes] ────────   8. Return PCM
  │                     │      (chunked or full)                as response
  │ Streams to speaker  │
  └─────────────────────┘
```

### What Your Backend Modification Looks Like

Your existing Python backend (conceptual):

```python
# BEFORE — PC mic and speaker
audio = record_from_pc_mic()           # ← touchpoint 1: input
transcript = speech_to_text(audio)
response_text = call_llm(transcript)
tts_output = text_to_speech(response_text)
play_on_pc_speaker(tts_output)         # ← touchpoint 2: output
```

Modified backend with Flask endpoint:

```python
from flask import Flask, request, Response
import io

app = Flask(__name__)

# Temporary storage for the latest TTS response.
# The mic upload request stores it; the speaker request retrieves it.
latest_response_pcm = None

@app.route('/api/conversation', methods=['POST'])
def conversation():
    global latest_response_pcm

    if len(request.data) > 0:
        # ── MIC UPLOAD REQUEST ──────────────────────────────────
        # ESP32 is sending recorded mic audio.
        # request.data = raw PCM: 16-bit signed, 16 kHz, mono
        #
        # The X-Audio-Format header confirms the format:
        #   "pcm;rate=16000;bits=16;channels=1"

        raw_pcm = request.data

        # ── YOUR EXISTING PIPELINE (unchanged) ──────────────────
        transcript = speech_to_text(raw_pcm)
        response_text = call_llm(transcript)
        tts_output = text_to_speech(response_text)

        # ── TRANSCODE TO RAW PCM ────────────────────────────────
        # The ESP32 speaker driver expects: s16le, 16 kHz, mono.
        # If your TTS returns wav/mp3/opus, convert here:
        latest_response_pcm = transcode_to_raw_pcm(tts_output)

        return Response('OK', status=200)

    else:
        # ── SPEAKER FETCH REQUEST ───────────────────────────────
        # ESP32 is requesting the audio response to play.
        # Return the stored PCM from the previous mic upload.

        if latest_response_pcm is None:
            return Response('No audio available', status=404)

        pcm = latest_response_pcm
        latest_response_pcm = None  # clear after serving

        return Response(pcm, status=200,
                        mimetype='application/octet-stream')

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000)
```

### Transcoding TTS Output

The ESP32 speaker driver expects **raw PCM**: 16-bit signed little-endian
(`s16le`), 16 kHz sample rate, mono, no headers. Most TTS engines output
wav, mp3, or opus — you need to strip headers and/or resample.

Using `pydub` (pip install pydub — requires ffmpeg installed):

```python
from pydub import AudioSegment
import io

def transcode_to_raw_pcm(tts_output_bytes, input_format='wav'):
    """Convert TTS output (wav/mp3/opus) to raw s16le PCM at 16 kHz mono."""
    audio = AudioSegment.from_file(io.BytesIO(tts_output_bytes),
                                   format=input_format)
    audio = audio.set_frame_rate(16000).set_channels(1).set_sample_width(2)
    return audio.raw_data
```

Using `scipy` (if TTS returns a numpy array + sample rate):

```python
import numpy as np
from scipy.signal import resample

def transcode_to_raw_pcm(samples, original_rate, target_rate=16000):
    """Resample float32 audio to 16 kHz s16le PCM."""
    if original_rate != target_rate:
        num_samples = int(len(samples) * target_rate / original_rate)
        samples = resample(samples, num_samples)
    pcm_int16 = np.clip(samples * 32767, -32768, 32767).astype(np.int16)
    return pcm_int16.tobytes()
```

If your TTS already outputs 16 kHz 16-bit mono PCM (or wav with those
parameters), you only need to strip the wav header (first 44 bytes):

```python
def strip_wav_header(wav_bytes):
    """Remove the 44-byte WAV header, returning raw PCM."""
    return wav_bytes[44:]
```

### STT Input Format

The mic audio arrives as raw PCM: 16-bit signed, 16 kHz, mono. Most STT
APIs accept this directly:

- **OpenAI Whisper API**: Expects a file upload (wav/mp3). Wrap the raw PCM
  in a wav header before sending:

  ```python
  import wave, io

  def wrap_pcm_as_wav(raw_pcm, sample_rate=16000, channels=1, sample_width=2):
      buf = io.BytesIO()
      with wave.open(buf, 'wb') as wf:
          wf.setnchannels(channels)
          wf.setsampwidth(sample_width)
          wf.setframerate(sample_rate)
          wf.writeframes(raw_pcm)
      buf.seek(0)
      return buf
  ```

- **Google Speech-to-Text**: Accepts raw linear16 PCM at 16 kHz directly.
- **Deepgram**: Accepts raw PCM with encoding specified in the API call.
- **Local Whisper (`openai-whisper` package)**: Load the raw PCM into a
  numpy array: `np.frombuffer(raw_pcm, dtype=np.int16).astype(np.float32) / 32768.0`

---

## Audio Format Contract

Both the mic and speaker drivers use the same format, controlled by the
`AUDIO_PRESET` define in each driver file. The default is speech mode:

| Parameter      | Value                     |
|----------------|---------------------------|
| Encoding       | Raw PCM (no container)    |
| Sample format  | 16-bit signed (`s16le`)   |
| Sample rate    | 16,000 Hz                 |
| Channels       | 1 (mono)                  |
| Byte rate      | 32,000 bytes/second       |
| Bit rate       | 256 kbps                  |

The ESP32 sends an `X-Audio-Format` header on both requests:

```
X-Audio-Format: pcm;rate=16000;bits=16;channels=1
```

The backend can parse this header to dynamically configure its STT and
TTS pipelines, or simply hard-code the format to match.

**Format mismatch produces noise, not errors.** The ESP32 does not validate
the audio format of the response. If the backend returns audio at the wrong
sample rate or encoding, the speaker plays garbage.

---

## Setup A: Wokwi Simulation

In simulation mode, the ESP32 runs in Wokwi (no physical hardware). Audio
input/output is routed through the Python audio bridge to your PC's real
mic and speakers. The backend runs on the same PC.

### Network Topology

```
  ┌─────────────────────────────────────────────────────────────────────┐
  │  YOUR PC                                                            │
  │                                                                     │
  │  ┌───────────────────┐        ┌────────────────────────────────┐    │
  │  │ audio_bridge.py   │        │ your_backend.py                │    │
  │  │ :8080             │        │ :5000                          │    │
  │  │                   │        │                                │    │
  │  │ GET  /mic         │        │ POST /api/conversation         │    │
  │  │ POST /speaker     │        │   body ≠ empty → process mic   │    │
  │  │ GET  /health      │        │   body = empty → return audio  │    │
  │  └────────▲──────────┘        └──────────▲─────────────────────┘    │
  │           │                              │                          │
  │           │ Wokwi host:                    │ Wokwi host:              │
  │           │ host.wokwi.internal:8080     │ host.wokwi.internal:5000 │
  │           │                              │                          │
  └───────────┼──────────────────────────────┼──────────────────────────┘
              │                              │
  ┌───────────┼──────────────────────────────┼──────────────────────────┐
  │  WOKWI    │                              │                          │
  │  ESP32    │                              │                          │
  │           │                              │                          │
  │  ┌────────┴──────────────┐  ┌──── ───────┴──────────────────────┐   │
  │  │ mic_capture_task      │  │ voice_assistant_task              │   │
  │  │ (MIC_SIMULATE=1)      │  │                                   │   │
  │  │                       │  │ mic_upload(BACKEND_MIC_URL)       │   │
  │  │ GET /mic from bridge  │  │   → POST to backend               │   │
  │  │ → captures PC mic     │  │                                   │   │
  │  │                       │  │ speaker_play_url(BACKEND_SPEAKER) │   │
  │  └───────────────────────┘  │   → POST to backend               │   │
  │                             │   → streams PCM to speaker task   │   │
  │  ┌───────────────────────┐  │                                   │   │
  │  │ speaker_playback_task │  └───────────────────────────────────┘   │
  │  │ (SPEAKER_SIMULATE=1)  │                                          │
  │  │                       │                                          │
  │  │ POST /speaker to      │                                          │
  │  │ bridge → plays on     │                                          │
  │  │ PC speakers           │                                          │
  │  └───────────────────────┘                                          │
  └──────────────────────────────────────────────────────────────────── ┘
```

There are **two independent network paths** from the ESP32:

1. **Audio bridge path** (simulation only):
   The mic capture task and speaker playback task use HTTP calls to the
   audio bridge to pipe I2S data through the PC's physical mic and
   speakers. This replaces the I2S hardware that doesn't exist in Wokwi.

2. **Backend path** (always):
   The voice assistant task uses HTTP calls to upload recorded audio and
   fetch response audio. This is the same path used on real hardware.

These two paths are independent — the audio bridge and the backend don't
talk to each other. They both talk to the ESP32.

### Step-by-Step Setup (Wokwi)

**1. Set the backend URLs and Wi-Fi in firmware**

All network configuration lives in `main/board_config.h`. Set the backend
URLs to point at your backend through the Wokwi gateway, and set the
Wi-Fi credentials for the simulated network:

```c
// Backend
#define BACKEND_MIC_URL         "http://host.wokwi.internal:5000/api/conversation"
#define BACKEND_SPEAKER_URL     "http://host.wokwi.internal:5000/api/conversation"

// Audio bridge (Wokwi simulation only)
#define AUDIO_BRIDGE_MIC_URL    "http://host.wokwi.internal:8080/mic"
#define AUDIO_BRIDGE_SPEAKER_URL "http://host.wokwi.internal:8080/speaker"

// Wi-Fi
#define WIFI_SSID               "Wokwi-GUEST"
#define WIFI_PASS               ""
```

`host.wokwi.internal` is a special hostname that Wokwi's DNS resolves
to the host PC's localhost. The ports must match the services running
on your PC (`5000` for the backend, `8080` for the audio bridge).

**2. Enable simulation mode**

In `main/board_config.h`, make sure both simulation flags are set to 1:

```c
#define MIC_SIMULATE            1
#define SPEAKER_SIMULATE        1
```

This routes I2S reads/writes through the audio bridge instead of hardware.

**3. Start the audio bridge**

```bash
pip install -r tools/audio_bridge/requirements.txt   # one time
python tools/audio_bridge/audio_bridge.py
```

This starts the bridge on `0.0.0.0:8080`. Verify with:

```bash
curl http://localhost:8080/health
```

**4. Start your backend**

```bash
python your_backend.py
```

Make sure it listens on `0.0.0.0:5000` (or whatever port you configured).
Binding to `0.0.0.0` (not `127.0.0.1`) is required so Wokwi's virtual
network can reach it.

**5. Build and run in Wokwi**

```bash
idf.py build
```

Then press F1 → **Wokwi: Start Simulator** in VS Code.

**6. Test the full loop**

- Press the push-to-talk button in the Wokwi UI (GPIO4 / D3)
- Speak into your PC mic (audio bridge captures it via the mic path)
- The voice assistant records, uploads PCM to your backend, your backend
  processes (STT → LLM → TTS), and returns PCM
- The speaker playback task receives the PCM and sends it to the audio
  bridge, which plays it on your PC speakers

### What Runs Where (Wokwi)

| Process              | Runs on    | Port  | Purpose                          |
|----------------------|------------|-------|----------------------------------|
| Wokwi ESP32          | VS Code    | —     | Firmware simulation              |
| `audio_bridge.py`    | Your PC    | 8080  | PC mic/speaker ↔ simulated I2S   |
| `your_backend.py`    | Your PC    | 5000  | STT → LLM → TTS processing      |

---

## Setup B: Real Hardware

On real hardware, the ESP32 has a physical I2S mic (e.g. INMP441) and I2S
speaker amp (e.g. MAX98357A). No audio bridge is needed — I2S handles
audio directly. The backend runs on a separate machine on the same network.

### Network Topology

```
                    Wi-Fi (same LAN)
                         │
  ┌──────────────────────┼─────────────────────────────────────────┐
  │                      │                                         │
  │  ┌───────────────────┴────────────────────────────────────┐    │
  │  │               ESP32-S3 XIAO                            │    │
  │  │                                                        │    │
  │  │  ┌─────────────┐    ┌────────────────────────┐         │    │
  │  │  │ mic_capture │    │ voice_assistant_task   │         │    │
  │  │  │ _task       │    │                        │         │    │
  │  │  │ I2S RX ← mic│    │ mic_upload() ──────────┼────┐    │    │
  │  │  └─────────────┘    │                        │    │    │    │
  │  │                     │ speaker_play_url() ────┼──┐ │    │    │
  │  │  ┌─────────────┐    └────────────────────────┘  │ │    │    │
  │  │  │ speaker_    │                                │ │    │    │
  │  │  │ playback    │◄── PCM from network task       │ │    │    │
  │  │  │ _task       │                                │ │    │    │
  │  │  │ I2S TX →amp │                                │ │    │    │
  │  │  └─────────────┘                                │ │    │    │
  │  │                                                 │ │    │    │
  │  └─────────────────────────────────────────────────┼─┼────┘    │
  │                                                    │ │         │
  │              HTTP over Wi-Fi                       │ │         │
  │                                                    │ │         │
  │  ┌─────────────────────────────────────────────────┼─┼────┐    │
  │  │  Backend Server (PC / Raspberry Pi / cloud)     │ │    │    │
  │  │  192.168.x.x:5000                               │ │    │    │
  │  │                                                 │ │    │    │
  │  │  POST /api/conversation  ◄──────────────────────┘ │    │    │
  │  │    body = empty → return stored PCM               │    │    │
  │  │                                                   │    │    │
  │  │  POST /api/conversation  ◄────────────────────────┘    │    │
  │  │    body = PCM → STT → LLM → TTS → store PCM            │    │
  │  │    return 200 OK                                       │    │
  │  │                                                        │    │
  │  └────────────────────────────────────────────────────────┘    │
  │                                                                │
  └────────────────────────────────────────────────────────────────┘
```

### Step-by-Step Setup (Real Hardware)

**1. Set the backend URLs and Wi-Fi in firmware**

All network configuration lives in `main/board_config.h`. Set the backend
URLs to your backend machine's LAN IP and your real Wi-Fi credentials:

```c
// Backend — use your server's LAN IP
#define BACKEND_MIC_URL         "http://192.168.1.100:5000/api/conversation"
#define BACKEND_SPEAKER_URL     "http://192.168.1.100:5000/api/conversation"

// Wi-Fi — your real network
#define WIFI_SSID               "YourNetworkName"
#define WIFI_PASS               "YourPassword"
```

Replace `192.168.1.100` with the actual IP address of the machine running
your backend. Use a static IP or DHCP reservation to prevent the IP from
changing. The audio bridge URLs can be left as-is — they are ignored when
simulation mode is off.

**2. Disable simulation mode**

In `main/board_config.h`, set both simulation flags to 0:

```c
#define MIC_SIMULATE            0
#define SPEAKER_SIMULATE        0
```

This makes the mic and speaker drivers use real I2S hardware.

**4. Wire the hardware**

Mic (INMP441) to XIAO ESP32-S3:

| INMP441 Pin | XIAO Pin | GPIO |
|-------------|----------|------|
| SCK (BCLK)  | D0       | 1    |
| WS (LRCLK)  | D1       | 2    |
| SD (DOUT)   | D2       | 3    |
| VDD         | 3V3      | —    |
| GND         | GND      | —    |
| L/R         | GND      | —    |

Speaker amp (MAX98357A) to XIAO ESP32-S3:

| MAX98357A Pin | XIAO Pin | GPIO |
|---------------|----------|------|
| BCLK          | D10/SDA  | 9    |
| LRC (WS)      | D7/SCL   | 10   |
| DIN           | D6       | 44   |
| VIN           | 3V3/5V   | —    |
| GND           | GND      | —    |

Push-to-talk button:

| Button       | XIAO Pin | GPIO |
|--------------|----------|------|
| One terminal | D3       | 4    |
| Other terminal | GND    | —    |

Pin assignments are defined in `main/board_config.h`.

**5. Start your backend on the server machine**

```bash
python your_backend.py
```

Must listen on `0.0.0.0:5000` so the ESP32 can reach it over the network.

**6. Build, flash, and run**

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

**7. Test**

- Press and hold the push-to-talk button (GPIO4), speak, release
- Monitor output shows: recording → upload → backend processing → playback
- You hear the response through the connected speaker

---

## End-to-End Data Flow — Both Setups

This is the complete lifecycle of one conversation turn, showing every
component and data transformation:

```
  ┌───────────────────────────────────────────────────────────────────┐
  │                    ONE CONVERSATION TURN                          │
  └───────────────────────────────────────────────────────────────────┘

  ┌──────────────┐
  │ 1. BUTTON    │  User presses PTT button (GPIO4)
  │    PRESS     │  voice_assistant_task detects button_pressed() == true
  └──────┬───────┘
         │ mic_start()
         ▼
  ┌──────────────┐
  │ 2. RECORD    │  mic_capture_task reads I2S RX (or audio bridge)
  │    MIC       │  gain scaling applied per-sample
  │              │  PCM written to mic stream buffer
  │              │  Accumulates s_captured_bytes
  └──────┬───────┘
         │ button released → mic_stop()
         ▼
  ┌──────────────┐
  │ 3. UPLOAD    │  mic_upload(BACKEND_MIC_URL)
  │    TO        │  Spawns mic_upload_task:
  │    BACKEND   │    HTTP POST to /api/conversation
  │              │    Content-Type: application/octet-stream
  │              │    Body: raw PCM from stream buffer
  │              │    Backend responds: 200 OK
  └──────┬───────┘
         │ mic state → DONE → IDLE
         ▼
  ┌──────────────┐
  │ 4. BACKEND   │  Your Python script receives the raw PCM:
  │    PROCESSES │    STT: raw PCM → text transcript
  │              │    LLM: transcript → response text
  │              │    TTS: response text → audio (wav/mp3/opus)
  │              │    Transcode: TTS output → raw s16le 16kHz mono PCM
  │              │    Store result in memory
  │              │    Return HTTP 200 OK
  └──────┬───────┘
         │
         ▼
  ┌──────────────┐
  │ 5. FETCH     │  speaker_play_url(BACKEND_SPEAKER_URL)
  │    RESPONSE  │  Spawns speaker_network_task:
  │              │    HTTP POST to /api/conversation (empty body)
  │              │    Backend returns: stored raw PCM
  │              │    PCM streamed into speaker stream buffer
  │              │    Pre-buffer fills → state = PLAYING
  └──────┬───────┘
         │
         ▼
  ┌──────────────┐
  │ 6. PLAYBACK  │  speaker_playback_task reads from stream buffer:
  │              │    Volume scaling applied per-sample
  │              │    I2S TX writes to amplifier (or audio bridge)
  │              │    Buffer drains → DRAINING → STOPPED → IDLE
  └──────┬───────┘
         │
         ▼
  ┌──────────────┐
  │ 7. READY     │  voice_assistant_task sees IDLE
  │              │  Loops back to step 1 — waiting for next button press
  └──────────────┘
```

---

## What You Modify vs. What You Keep

### On the Backend (your Python script)

| Component                        | Action     | Details                                         |
|----------------------------------|------------|-------------------------------------------------|
| PC mic recording                 | **Remove** | Replaced by receiving `request.data` (raw PCM)  |
| Spacebar / push-to-talk listener | **Remove** | The ESP32 button triggers recording, not the PC |
| STT pipeline                     | **Keep**   | Fed with raw PCM from the HTTP request body     |
| LLM call                         | **Keep**   | Receives transcript from STT, unchanged         |
| TTS pipeline                     | **Keep**   | Generates audio response, unchanged             |
| PC speaker playback              | **Remove** | Replaced by returning PCM in the HTTP response  |
| Transcode to raw PCM             | **Add**    | Convert TTS output to s16le, 16 kHz, mono       |
| Flask endpoint                   | **Add**    | Single POST endpoint handling both request types|

### On the ESP32 Firmware

All network configuration is centralised in `main/board_config.h`:

| Define in `board_config.h`  | Action           | Details                                       |
|-----------------------------|------------------|-----------------------------------------------|
| `BACKEND_MIC_URL`           | **Set URL**      | Point to your backend's address and port      |
| `BACKEND_SPEAKER_URL`       | **Set URL**      | Same URL as mic (single endpoint)             |
| `AUDIO_BRIDGE_MIC_URL`      | **Set URL**      | Audio bridge mic endpoint (Wokwi only)        |
| `AUDIO_BRIDGE_SPEAKER_URL`  | **Set URL**      | Audio bridge speaker endpoint (Wokwi only)    |
| `WIFI_SSID`                 | **Set SSID**     | Wi-Fi network name                            |
| `WIFI_PASS`                 | **Set password** | Wi-Fi password (empty for Wokwi-GUEST)        |

| `MIC_SIMULATE`              | **Set 0 or 1**   | 1 for Wokwi, 0 for real hardware              |
| `SPEAKER_SIMULATE`          | **Set 0 or 1**   | 1 for Wokwi, 0 for real hardware              |

Everything else (drivers, tasks, state machines) stays as-is.

---

## Timing and Latency

Understanding the latency budget helps set expectations:

```
  Push-to-talk conversation turn (typical timing):

  ├── Recording ──────────── 1–5 s ──────────────────┤  (user speaks)
  ├── Upload ─────────────── 0.1–0.5 s ──────────────┤  (depends on audio length)
  ├── Backend processing:                            │
  │   ├── STT ────────────── 0.5–2 s ────────────────┤  (API latency)
  │   ├── LLM ────────────── 0.5–3 s ────────────────┤  (API latency)
  │   └── TTS ────────────── 0.3–1 s ────────────────┤  (API latency)
  ├── Fetch + pre-buffer ─── 0.1–0.3 s ──────────────┤  (network + 125ms prebuffer)
  ├── Playback ──────────── 1–10 s ──────────────────┤  (depends on response)
  └── Total turn ─────────── 3–20 s ─────────────────┘
```

The ESP32 `speaker_network_task` has a 30-second HTTP timeout. If your
backend takes longer than 30 seconds to respond (STT + LLM + TTS combined),
the connection will time out and the speaker will report an error. Adjust
`timeout_ms` in `speaker_driver.c` if needed.

The `mic_upload_task` also has a 30-second timeout for the upload.

### Streaming Response (Lower Latency)

The speaker driver supports **chunked transfer encoding**. If your backend
can start streaming TTS audio before the entire response is generated
(e.g. streaming TTS), it can send PCM chunks as they become available:

```python
from flask import Flask, request, Response

@app.route('/api/conversation', methods=['POST'])
def conversation():
    if len(request.data) > 0:
        # Process mic audio...
        return Response('OK', status=200)
    else:
        def generate_pcm():
            for chunk in streaming_tts(response_text):
                yield transcode_chunk_to_pcm(chunk)

        return Response(generate_pcm(), status=200,
                        mimetype='application/octet-stream')
```

With streaming, the speaker starts playing as soon as the pre-buffer
threshold (4 KB = 125 ms of audio) is filled — even while the backend
is still generating the rest of the response.

---

## Troubleshooting

| Symptom                            | Likely cause                               | Fix                                                            |
|------------------------------------|--------------------------------------------|----------------------------------------------------------------|
| `HTTP open failed`                 | Backend not reachable                      | Check IP, port, firewall. Ping from ESP32 network.             |
| `Backend returned HTTP 4xx/5xx`    | Backend error                              | Check backend logs. Verify endpoint path matches.              |
| Speaker plays noise/static         | Audio format mismatch                      | Ensure TTS output is transcoded to s16le, 16 kHz, mono.        |
| Speaker plays silence              | Backend returned empty body                | Check that speaker fetch returns PCM, not empty 200.           |
| `No audio captured`               | Mic not recording                          | Check I2S wiring. Check `MIC_SIMULATE` in board_config.h.      |
| Audio cuts out / choppy            | Network too slow for real-time streaming   | Increase `RING_BUFFER_SIZE` in speaker_driver.c.               |
| `HTTP read error` during playback  | Backend closed connection early            | Ensure backend sends complete PCM before closing.              |
| Wokwi can't reach backend          | Backend bound to 127.0.0.1                 | Bind to `0.0.0.0` so Wokwi gateway can route to it.           |
| Audio bridge works but backend doesn't | Different ports, wrong hostname         | Audio bridge uses :8080, backend uses :5000. Both via host.wokwi.internal. |

---

## Quick Reference — Files to Touch

```
  YOUR BACKEND (Python):
  ──────────────────────
  your_backend.py
    └── Add Flask endpoint /api/conversation
    └── Remove PC mic recording
    └── Remove PC speaker playback
    └── Add transcode_to_raw_pcm() helper
    └── Keep STT / LLM / TTS pipeline unchanged

  ESP32 FIRMWARE:
  ───────────────
  main/board_config.h           ← All network config in one place:
    ├── BACKEND_MIC_URL             backend conversation endpoint
    ├── BACKEND_SPEAKER_URL         backend conversation endpoint
    ├── AUDIO_BRIDGE_MIC_URL        audio bridge mic (Wokwi only)
    ├── AUDIO_BRIDGE_SPEAKER_URL    audio bridge speaker (Wokwi only)
    ├── WIFI_SSID                   Wi-Fi network name
    └── WIFI_PASS                   Wi-Fi password

    ├── MIC_SIMULATE                simulation mode (1 = Wokwi, 0 = hardware)
    └── SPEAKER_SIMULATE            simulation mode (1 = Wokwi, 0 = hardware)
```

No other firmware files need to be modified.

---

## FAQ — Backend Integration

### Q: What are the two URLs (`host.wokwi.internal:5000` and `localhost:8080`) and which service does each belong to?

They belong to two completely separate services that serve different
purposes:

- **`host.wokwi.internal:5000`** reaches your **Python backend**
  (`your_backend.py`). This is the brain of the system — it receives mic
  audio, runs it through STT, sends the transcript to an LLM, generates
  a TTS response, and returns the audio. The hostname `host.wokwi.internal`
  is resolved by Wokwi's DNS to the host PC's localhost, so the simulated
  ESP32 can reach services running on the host machine. Port `5000` is the
  default Flask port.

- **`localhost:8080`** reaches the **audio bridge** (`audio_bridge.py`).
  This service only exists for Wokwi simulation. It acts as a stand-in for
  real I2S hardware by routing audio between the simulated ESP32 and your
  PC's physical microphone and speakers. It exposes `/mic`, `/speaker`,
  and `/health` endpoints. It has nothing to do with the conversation
  pipeline — it is purely an I/O proxy.

The two services are independent. The ESP32 talks to both, but they never
talk to each other.

### Q: What needs to be configured to connect the ESP32 to the backend?

**On the ESP32 (firmware side):**

All network settings are centralised in `main/board_config.h`:

- `BACKEND_MIC_URL` / `BACKEND_SPEAKER_URL` — point to your backend's
  address and port. Both can be the same URL for single-endpoint mode.
- `AUDIO_BRIDGE_MIC_URL` / `AUDIO_BRIDGE_SPEAKER_URL` — the audio bridge
  endpoints. Only used during Wokwi simulation; ignored on real hardware.
- `WIFI_SSID` / `WIFI_PASS` — the Wi-Fi network the ESP32 joins.
- `MIC_SIMULATE` / `SPEAKER_SIMULATE` — set to 1 for Wokwi, 0 for real
  hardware.

**On the backend (Python side):**

- Bind the Flask server to `0.0.0.0` (all network interfaces) on port
  `5000`. The port must match the port in the firmware's backend URLs.
- Expose a single `POST /api/conversation` endpoint. When the request body
  contains audio data, treat it as a mic upload (run STT → LLM → TTS and
  store the result). When the body is empty, return the previously stored
  TTS audio as raw PCM.
- Add a transcoding step that converts your TTS engine's output (wav, mp3,
  opus, etc.) into raw PCM: 16-bit signed little-endian, 16 kHz, mono —
  the format the ESP32 speaker driver expects.
- Remove any code that records from the PC microphone or plays audio on
  the PC speakers — the ESP32 handles both of those now.
- Leave the STT, LLM, and TTS pipelines themselves untouched; only the
  input source and output destination change.

### Q: Why must the backend bind to `0.0.0.0` instead of `127.0.0.1`?

`127.0.0.1` (loopback) only accepts connections that originate from the
same machine. The ESP32 — whether simulated in Wokwi or running on real
hardware — is a different network host:

- **Wokwi:** The simulated ESP32 reaches your PC through the hostname
  `host.wokwi.internal`. Traffic arriving through the gateway enters
  your PC on a network interface, not on loopback. A server bound to
  `127.0.0.1` will refuse the connection.
- **Real hardware:** The ESP32 connects over Wi-Fi to your PC's LAN IP
  (e.g. `192.168.1.100`). Again, this is a network-interface connection,
  not loopback.

Binding to `0.0.0.0` tells the OS to listen on every available interface
(loopback, Wi-Fi, Ethernet, Wokwi's virtual bridge, etc.), so the server
accepts connections regardless of which interface they arrive on. This is
the simplest way to ensure the ESP32 can always reach the backend.
