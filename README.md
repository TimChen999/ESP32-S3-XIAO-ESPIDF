# Cellular Modem Simulator — ESP32-S3 (Seeed XIAO)

A cellular modem simulator that runs entirely on the ESP32-S3. Two UARTs are cross-wired: one runs the modem driver (AT commands + PPP), the other runs a simulated modem that responds with realistic AT responses and PPP frames.

---

## Setup via VS Code (recommended)

You can do everything through VS Code without installing ESP-IDF separately.

### 1. Install the ESP-IDF extension

1. Open VS Code (or Cursor).
2. Go to **Extensions** (Ctrl+Shift+X).
3. Search for **ESP-IDF** and install **Espressif IDF** (`espressif.esp-idf-extension`).

### 2. Configure ESP-IDF (first time only)

1. Press **Ctrl+Shift+P** to open the Command Palette.
2. Run **ESP-IDF: Configure ESP-IDF Extension**.
3. Choose **Express** installation (recommended).
4. Select **ESP-IDF v5.x** and **Xtensa (ESP32)** toolchain.
5. Choose an install path (e.g. `C:\Espressif` on Windows).
6. Wait for the download and installation to finish.

The extension installs ESP-IDF and the toolchain for you. No manual `export` scripts needed.

### 3. Open the project and set target

1. Open this project folder in VS Code.
2. Press **Ctrl+Shift+P** → **ESP-IDF: Set Espressif Device Target**.
3. Select **esp32s3**.

### 4. Build, flash, and monitor

Use the **ESP-IDF** buttons in the bottom status bar, or:

| Action   | Command Palette                          |
|----------|------------------------------------------|
| Build    | **ESP-IDF: Build your project**         |
| Flash    | **ESP-IDF: Flash your project**         |
| Monitor  | **ESP-IDF: Monitor your project**       |
| Full     | **ESP-IDF: Build, Flash and Monitor**   |

For flash/monitor, select your board’s COM port when prompted.

---

## Simulate with Wokwi

You can run the firmware in the Wokwi simulator (no hardware needed).

### 1. Install the Wokwi extension

1. Go to **Extensions** (Ctrl+Shift+X).
2. Search for **Wokwi** and install **Wokwi Simulator** (`Wokwi.wokwi-vscode`).

### 2. Activate a free license

1. Press **Ctrl+Shift+A** (or **Cmd+Shift+A** on macOS).
2. Choose **Wokwi: Request New License**.
3. Complete the activation in your browser.

### 3. Build the firmware

Build the project first (via ESP-IDF extension or `idf.py build`). Wokwi uses the binaries in `build/`.

### 4. Start the simulator

1. Press **F1** (or **Ctrl+Shift+P**).
2. Run **Wokwi: Start Simulator**.

The simulator uses `diagram.json` (circuit) and `wokwi.toml` (firmware paths and networking). The diagram cross-wires UART1 and UART2 on the XIAO ESP32-S3 as required by the modem simulator.

**Important:** `wokwi.toml` must include `[net] enable = true` so the simulated ESP32 can reach services on your PC via the `host.wokwi.internal` hostname. This is already configured in the project.

### 5. Audio bridge — use your PC mic and speakers (optional)

When `MIC_SIMULATE=1` and `SPEAKER_SIMULATE=1` are set in `main/board_config.h`, the mic and speaker drivers route audio through a Python HTTP bridge running on your PC instead of using I2S hardware. This lets you have a real voice conversation through the Wokwi simulator.

**Install the bridge dependencies (one time):**

```bash
pip install -r tools/audio_bridge/requirements.txt
```

**Start the bridge before launching Wokwi:**

```bash
python tools/audio_bridge/audio_bridge.py
```

The bridge opens two endpoints on `localhost:8080`:
- `GET /mic` — records a chunk from your PC microphone, returns raw PCM
- `POST /speaker` — receives raw PCM, plays it on your PC speakers

The ESP32 firmware calls these endpoints from the simulate branches of the capture and playback loops. The bridge URL defaults to `http://host.wokwi.internal:8080` (Wokwi's hostname for reaching the host PC). Override by changing `AUDIO_BRIDGE_MIC_URL` and `AUDIO_BRIDGE_SPEAKER_URL` in `main/board_config.h` if your setup uses a different address.

**Optional bridge flags:**

```bash
python tools/audio_bridge/audio_bridge.py --list-devices     # show audio devices
python tools/audio_bridge/audio_bridge.py --input-device 2   # pick a specific mic
python tools/audio_bridge/audio_bridge.py --port 9090        # custom port
```

**Test the bridge independently (no Wokwi needed):**

```bash
curl http://localhost:8080/health
curl http://localhost:8080/mic -o chunk.raw
curl -X POST http://localhost:8080/speaker --data-binary @chunk.raw
```

---

## Backend Integration Configuration

The voice assistant firmware connects to the Python backend (Character AI push-to-talk) over HTTP. This section lists every config that links the two systems and how to adjust them.

### Configs set for Wokwi simulation (current)

All deployment-specific settings are centralised in `main/board_config.h`:

| Config | Current value | Purpose |
|--------|---------------|---------|
| `MIC_SIMULATE` | `1` | Routes mic I2S reads through the PC audio bridge |
| `SPEAKER_SIMULATE` | `1` | Routes speaker I2S writes through the PC audio bridge |
| `WIFI_SSID` | `Wokwi-GUEST` | Wokwi's built-in simulated Wi-Fi AP |
| `WIFI_PASS` | `""` (empty) | No password for Wokwi-GUEST |
| `BACKEND_MIC_URL` | `http://host.wokwi.internal:5000/api/conversation` | Endpoint the ESP POSTs recorded PCM to |
| `BACKEND_SPEAKER_URL` | `http://host.wokwi.internal:5000/api/conversation` | Endpoint the ESP POSTs (empty body) to fetch reply PCM |
| `AUDIO_BRIDGE_MIC_URL` | `http://host.wokwi.internal:8080/mic` | Audio bridge mic endpoint (Wokwi only) |
| `AUDIO_BRIDGE_SPEAKER_URL` | `http://host.wokwi.internal:8080/speaker` | Audio bridge speaker endpoint (Wokwi only) |

The hostname `host.wokwi.internal` is resolved by Wokwi's DNS to route traffic to the host PC's localhost. Port `5000` must match the backend's `esp_audio.port` setting. Both URLs point to the same `/api/conversation` endpoint — the backend distinguishes mic upload (body has audio) from speaker fetch (body is empty).

### Matching backend config (must be set in the backend project)

The backend must have these set in its `.env` (or equivalent):

| Backend config | Value | Why |
|----------------|-------|-----|
| `PTT_ESP_AUDIO__ENABLED` | `true` | Starts the Flask conversation server that the ESP connects to |
| `PTT_ESP_AUDIO__HOST` | `0.0.0.0` | Binds to all interfaces so Wokwi gateway / Wi-Fi can reach it |
| `PTT_ESP_AUDIO__PORT` | `5000` | Must match the port in `BACKEND_MIC_URL` / `BACKEND_SPEAKER_URL` |

### Configuring for real hardware (production)

When deploying on a physical ESP32-S3 XIAO with real I2S mic and speaker,
change these values in `main/board_config.h`:

| Config | Change to | Why |
|--------|-----------|-----|
| `MIC_SIMULATE` | `0` | Uses real I2S hardware instead of the audio bridge |
| `SPEAKER_SIMULATE` | `0` | Uses real I2S hardware instead of the audio bridge |
| `WIFI_SSID` | Your Wi-Fi network name | The real AP the ESP connects to |
| `WIFI_PASS` | Your Wi-Fi password | WPA2 passphrase |
| `BACKEND_MIC_URL` | `http://<BACKEND_LAN_IP>:5000/api/conversation` | Replace `host.wokwi.internal` with the backend server's actual LAN IP (e.g. `192.168.1.100`) |
| `BACKEND_SPEAKER_URL` | `http://<BACKEND_LAN_IP>:5000/api/conversation` | Same IP as mic URL |

Additional production considerations:

- **Static IP or DHCP reservation** — assign a fixed LAN IP to the backend server so the firmware URL doesn't go stale.
- **Firewall** — the backend machine must allow inbound TCP on port 5000 from the ESP's subnet.
- **Backend bind address** — keep `PTT_ESP_AUDIO__HOST=0.0.0.0` so the server listens on the Wi-Fi interface (not just loopback).
- **PTT button** — `PTT_BUTTON_ENABLED` in `voice_assistant.c` is `1` by default, which requires a physical momentary button on GPIO4. Set to `0` for auto-record mode during bench testing.
- **Audio format** — the backend must return raw PCM: 16-bit signed little-endian, 16 kHz, mono. Any format mismatch produces noise, not errors.

See `Docs/Audio Docs/BACKEND_INTEGRATION.md` for the full protocol specification, network diagrams, and troubleshooting table.

---

## Build via command line (optional)

If you prefer the terminal or have ESP-IDF installed separately:

### 1. Install ESP-IDF (first time only)

Download and run the ESP-IDF installer, or clone the repo manually.
Then run the install script to set up the toolchains and Python virtual environment:

```bash
# Linux/macOS
./install.sh

# Windows (PowerShell)
C:\esp\v5.5.3\esp-idf\install.ps1

# Windows (cmd)
C:\esp\v5.5.3\esp-idf\install.bat
```

> **Re-run the install script** if you update Python, move folders, or see
> errors about a missing Python virtual environment.

### 2. Export the environment (every new terminal)

The export script adds the ESP-IDF toolchain and `idf.py` to your `PATH`
for the current terminal session. You must run it every time you open a
new terminal window:

```bash
# Linux/macOS
. $HOME/esp/esp-idf/export.sh

# Windows (PowerShell)
C:\esp\v5.5.3\esp-idf\export.ps1

# Windows (cmd)
C:\esp\v5.5.3\esp-idf\export.bat
```

After this, `idf.py` should be available.

### 3. Build and flash

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

## Project structure

```
├── main/
│   ├── main.c          # app_main, task creation
│   ├── sim_modem.c     # Simulated modem (AT + PPP)
│   ├── sim_modem.h
│   ├── modem_driver.c  # Modem driver (AT + lwIP PPP)
│   └── modem_driver.h
├── CMakeLists.txt
├── sdkconfig.defaults  # Seeed XIAO ESP32-S3 config
└── README.md
```

## Pin wiring (Seeed XIAO)

| Function          | Driver UART1  | Sim Modem UART2 |
|-------------------|---------------|------------------|
| TX                | GPIO1 (D0)    | GPIO6 (D5)       |
| RX                | GPIO2 (D1)    | GPIO7 (D8)       |
| RTS (flow ctrl)   | GPIO3 (D2)    | GPIO5 (D4)       |
| CTS (flow ctrl)   | GPIO4 (D3)    | GPIO8 (D9)       |

**Physical wiring:** Driver TX → Modem RX, Modem TX → Driver RX. Cross-connect UART1 and UART2.

**Console:** UART0 (USB Serial/JTAG) — `printf` and `idf.py monitor` use this.

