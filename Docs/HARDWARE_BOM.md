# Hardware Bill of Materials

What you need to build and run this voice assistant project end to end.

---

## Part 1 — Required for Current Functionality

These are the components needed to run the firmware as-is: push-to-talk voice
conversations over Wi-Fi (record → upload → play response).

### 1. ESP32-S3 Development Board

The firmware targets the **Seeed Studio XIAO ESP32S3** form factor and pinout.
Any ESP32-S3 board will work if you remap the GPIO pins in the source code.

| Board | Notes |
|-------|-------|
| **Seeed XIAO ESP32S3** | Default target. Tiny 21×17.5 mm form factor. Has USB-C, 8 MB flash, 8 MB PSRAM, built-in 2.4 GHz Wi-Fi + BLE 5.0. No built-in mic or speaker. |
| **Seeed XIAO ESP32S3 Sense** | Same as above but adds an onboard PDM microphone (MSM261S4030H0) and camera connector. The onboard mic uses PDM mode, which requires different I2S init code (`i2s_channel_init_pdm_rx_mode`). The current firmware uses standard I2S mode, so the onboard mic will **not** work without code changes. External I2S mic still works fine on this board. |
| **ESP32-S3-DevKitC-1** (Espressif) | Full-size dev board with 44 GPIOs broken out. More pins available, easier to prototype. Same ESP32-S3 chip. Remap pin defines in `mic_driver.c` and `speaker_driver.c`. |
| **ESP32-S3-DevKitM-1** (Espressif) | Compact module-based dev board. Same chip, fewer breakout pins than DevKitC but more than XIAO. |
| **Waveshare ESP32-S3-Zero** | XIAO-sized alternative with USB-C. Check the pinout — GPIO numbers differ from XIAO. |

**Minimum requirements:** ESP32-S3 with at least 8 MB flash, Wi-Fi, and 7 free GPIO pins.

### 2. I2S MEMS Microphone

The firmware captures audio via the I2S standard (Philips) protocol at 16 kHz / 16-bit / mono.
Any I2S MEMS microphone that outputs standard I2S will work. PDM-only mics will not work
without code changes.

| Microphone | Interface | SNR | Price Range | Notes |
|------------|-----------|-----|-------------|-------|
| **INMP441** (breakout board) | I2S | 61 dB | $1–3 | Most common choice for ESP32 projects. Cheap, widely available, well-documented. This is what the code comments reference. |
| **ICS-43434** (breakout board) | I2S | 65 dB | $3–6 | Higher SNR than INMP441 — cleaner recordings with less background noise. Good upgrade for better STT accuracy. |
| **SPH0645LM4H** (Adafruit breakout) | I2S | 65 dB | $5–7 | Adafruit sells a ready-made breakout (product ID 3421). Same I2S wiring as INMP441. |
| **MSM261S4030H0** | PDM | 61 dB | — | Built into the XIAO ESP32S3 Sense. **Not compatible** with current firmware (needs PDM mode init). Mentioned for completeness. |

**Wiring (INMP441 example):**

| Mic Pin | ESP32-S3 GPIO | XIAO Pad | Firmware Define |
|---------|---------------|----------|-----------------|
| SCK     | GPIO 1        | D0       | `MIC_I2S_BCLK_PIN` |
| WS      | GPIO 2        | D1       | `MIC_I2S_WS_PIN` |
| SD      | GPIO 3        | D2       | `MIC_I2S_DIN_PIN` |
| VDD     | 3.3V          | 3V3      | — |
| GND     | GND           | GND      | — |
| L/R     | GND           | GND      | Selects left channel (mono) |

### 3. I2S Amplifier + Speaker

The firmware outputs audio via I2S standard (Philips) protocol at 16 kHz / 16-bit / mono.
It targets I2S-only amplifiers that need no I2C configuration — just 3 signal wires plus power.
Codec chips (ES8311, WM8960) are **not** supported.

#### Amplifier Boards

| Amplifier | Class | Power | Price Range | Notes |
|-----------|-------|-------|-------------|-------|
| **MAX98357A** (breakout board) | Class D | 3.2 W @ 4 Ω | $2–5 | Most popular choice for ESP32 I2S projects. Adafruit (product ID 3006) and generic boards available. Mono. Built-in DAC. No I2C needed. |
| **NS4168** (breakout board) | Class D | 3 W @ 4 Ω | $1–3 | Very similar to MAX98357A. Common on AliExpress boards. Same wiring. |
| **PCM5102A** (breakout board) | Hi-Fi DAC | Line-level out | $3–8 | High-quality DAC, but outputs line-level — needs a separate power amplifier and speaker. Overkill for a voice assistant but great audio quality. |
| **UDA1334A** (Adafruit breakout) | DAC | Line-level out | $6–8 | Adafruit product ID 3678. Line-level output, needs external amp. |

**Recommendation:** MAX98357A or NS4168 — they include the DAC and amplifier in one board, just add a speaker.

#### Speakers

| Speaker | Impedance | Power | Notes |
|---------|-----------|-------|-------|
| **4 Ω, 3 W mini speaker** | 4 Ω | 3 W | Best match for MAX98357A/NS4168. Small enough for portable builds. |
| **8 Ω, 2 W mini speaker** | 8 Ω | 2 W | Also works. Lower power output from the amp at 8 Ω (about half). |
| Any small speaker 4–8 Ω | 4–8 Ω | 1–5 W | Anything in this range will produce audible speech output. |

**Wiring (MAX98357A example):**

| Amp Pin | ESP32-S3 GPIO | XIAO Pad | Firmware Define |
|---------|---------------|----------|-----------------|
| BCLK    | GPIO 9        | D10/SDA  | `I2S_BCLK_PIN` |
| LRC     | GPIO 10       | D7/SCL   | `I2S_WS_PIN` |
| DIN     | GPIO 44       | D6       | `I2S_DOUT_PIN` |
| VIN     | 3.3V or 5V    | 3V3/5V   | — (check amp board's voltage range) |
| GND     | GND           | GND      | — |
| GAIN    | (leave unconnected or tie to GND/VDD for gain selection) | — | — |

Speaker connects to the amp board's output terminals (+ and −).

### 4. Push-to-Talk Button

A simple momentary push button. The firmware uses GPIO4 with an internal pull-up resistor —
pressing the button pulls the pin to GND.

| Component | Notes |
|-----------|-------|
| **Any momentary tactile switch** | 6×6 mm or 12×12 mm tactile buttons work fine. Two legs: one to GPIO4 (D3 on XIAO), one to GND. |
| Breadboard jumper wire | In a pinch, you can touch a wire from D3 to GND to simulate a button press. |

**Wiring:** `GPIO 4 (D3) ←→ button ←→ GND`. No external resistor needed (internal pull-up enabled in firmware).

### 5. Wi-Fi Network

The device connects to a Wi-Fi access point in station (client) mode. Currently hardcoded
to `Wokwi-GUEST` with no password (for simulator testing). For real hardware, change
`WIFI_SSID` and `WIFI_PASS` in `wifi_driver.c`.

| Requirement | Details |
|-------------|---------|
| 2.4 GHz Wi-Fi | ESP32-S3 supports 802.11 b/g/n on 2.4 GHz only (no 5 GHz). |
| Internet access | The device needs to reach your backend server for the voice conversation loop. |

### 6. Voice Assistant Backend Server

The firmware sends recorded audio to a backend and plays back the response. The backend
handles STT (speech-to-text), LLM (language model), and TTS (text-to-speech).

| Component | Details |
|-----------|---------|
| **Backend server** | Must expose two HTTP endpoints: one to receive mic audio (`POST /api/listen`), one to return audio response (`POST /api/speak`). URLs are configured in `voice_assistant.c`. |
| **STT engine** | OpenAI Whisper API, Google Speech-to-Text, Deepgram, or self-hosted Whisper. The mic sends raw PCM (s16le, 16 kHz, mono). |
| **LLM** | OpenAI GPT, Anthropic Claude, local LLM — whatever your backend uses to generate a text response. |
| **TTS engine** | Must output raw PCM (s16le, 16 kHz, mono) to match the speaker driver's format. ElevenLabs, Google Cloud TTS, OpenAI TTS, Piper TTS (local), etc. |

### 7. USB-C Cable

For flashing firmware and serial monitoring during development.

| Cable | Notes |
|-------|-------|
| USB-C data cable | Must support data transfer (not charge-only). Most USB-C cables work. |

### 8. Breadboard + Jumper Wires

For prototyping connections between the ESP32, mic, amp, button, and power.

| Component | Notes |
|-----------|-------|
| Half-size or full-size breadboard | Any standard solderless breadboard. |
| Male-to-male jumper wires | For breadboard connections. |
| Male-to-female jumper wires | If your breakout boards have pin headers. |

---

## Part 2 — Additional Hardware for a Self-Sufficient Device

To turn this from a dev-bench prototype into a standalone device that doesn't rely on
a computer for power or network connectivity.

### 9. Power Supply (Replace USB from Computer)

The ESP32-S3 plus audio peripherals draw roughly 150–300 mA during active Wi-Fi + audio playback,
with peaks up to ~500 mA.

#### Option A: Lithium Battery + Charger (Portable)

| Component | Notes |
|-----------|-------|
| **3.7V LiPo battery** (500–2000 mAh) | Single-cell lithium polymer. The XIAO ESP32S3 has built-in battery pads on the back that accept a 3.7V LiPo directly — it charges over USB-C and powers the board when unplugged. 1000 mAh gives roughly 3–5 hours of intermittent use. |
| **TP4056-based charger board** (if not using XIAO) | $0.50–2. Charges a LiPo cell from USB. Not needed for XIAO (built-in), but needed for DevKitC or other boards without battery management. |
| **3.3V voltage regulator** (if not using XIAO) | Most dev boards already have an onboard regulator. Only needed if you're building a custom PCB. |

#### Option B: USB Power Adapter (Stationary)

| Component | Notes |
|-----------|-------|
| **5V USB-C wall adapter** (1A+) | Any phone charger with USB-C. Powers the board indefinitely. Simplest option for a desk or wall-mounted assistant. |
| **5V USB-C power bank** | Portable USB battery. Some power banks shut off at low current draw — look for ones with a "low current mode" or "always on" feature. |

#### Option C: Direct DC Power (Embedded)

| Component | Notes |
|-----------|-------|
| **5V regulated power supply** | For embedding in a permanent installation. Feed 5V into the board's 5V pin (bypass USB). |
| **3.3V regulated supply** | If powering the 3.3V rail directly, bypass the onboard regulator. Be precise — ESP32-S3 is sensitive to supply voltage. |

**Recommendation:** For a portable voice assistant, use the XIAO ESP32S3 with a 3.7V LiPo
soldered to its battery pads. For a stationary assistant, a USB-C wall adapter is simplest.

### 10. Enclosure

| Option | Notes |
|--------|-------|
| **3D-printed case** | Design one for your specific component layout. Thingiverse/Printables have XIAO cases. Add cutouts for the mic, speaker grille, and button. |
| **Small project box** (ABS/plastic) | 80×50×25 mm or similar. Drill holes for speaker, mic, button, and USB port. |
| **No enclosure** | Fine for prototyping. Exposed electronics on a breadboard. |

### 11. Antenna (Usually Not Needed)

| Scenario | Notes |
|----------|-------|
| **Built-in PCB antenna** (default) | The XIAO ESP32S3 and most dev boards have an onboard PCB antenna. Sufficient for most indoor Wi-Fi ranges (10–30 m). |
| **External antenna + U.FL pigtail** | If the device is inside a metal enclosure or far from the router. The XIAO ESP32S3 has a U.FL connector for an external antenna (requires moving a 0-ohm resistor to switch from PCB to external antenna). |

---

## Part 3 — Optional Upgrades

These aren't required but improve the device as a finished product.

### LED Indicator

| Component | Notes |
|-----------|-------|
| Single RGB LED (WS2812B / NeoPixel) | Show device state: blue = listening, green = processing, red = error. Requires 1 GPIO pin and a small code addition. |
| Simple LED + resistor | Even simpler — just an on/off indicator. |

### OLED Display

| Component | Notes |
|-----------|-------|
| **SSD1306 0.96" OLED** (I2C) | Show status text, transcription, volume level. Uses I2C (SDA/SCL). Would conflict with the current speaker pin assignments on D10/D7 (which are the default I2C pins), so you'd need to remap either the speaker or I2C pins. |

### Physical Volume Control

| Component | Notes |
|-----------|-------|
| **Potentiometer** (10 kΩ) + ADC | Read analog value, map to 0.0–1.0 volume. Connect to any ADC-capable GPIO. Requires a small code addition to read the ADC and call `speaker_set_volume()`. |
| **Rotary encoder** | Digital volume control with click. Two GPIO pins. |

### Cellular Modem (Alternative to Wi-Fi)

The firmware already has modem driver code (commented out in `main.c`) for connecting
via a cellular modem instead of Wi-Fi. This would make the device truly independent of
any local network.

| Component | Notes |
|-----------|-------|
| **SIM800L / SIM7000 / SIM7600** module | GSM/LTE modem module. Connects via UART. Requires a SIM card with a data plan. Uses GPIO pins D0–D5 + D8–D9 for UART (conflicts with mic pins — can't use both simultaneously). |
| **SIM card** with data plan | Any IoT or mobile data SIM. Needs HTTP connectivity to reach the backend. |

---

## Quick Shopping List (Minimum Viable Device)

For the fastest path to a working push-to-talk voice assistant on real hardware:

| # | Component | Example Product | Est. Cost |
|---|-----------|-----------------|-----------|
| 1 | Seeed XIAO ESP32S3 | [Seeed Studio](https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html) | $8 |
| 2 | INMP441 I2S MEMS Microphone breakout | Search "INMP441 I2S microphone" on Amazon/AliExpress | $2 |
| 3 | MAX98357A I2S Amplifier breakout | Search "MAX98357A I2S amplifier" or Adafruit #3006 | $4 |
| 4 | 4 Ω 3 W mini speaker | Search "4 ohm 3W mini speaker" | $1 |
| 5 | Tactile push button (6×6 mm) | Any momentary switch | $0.10 |
| 6 | Breadboard + jumper wires | Half-size breadboard + dupont wires | $3 |
| 7 | USB-C data cable | — | $3 |
| | **Total (bench prototype)** | | **~$21** |

**Add for standalone operation:**

| # | Component | Est. Cost |
|---|-----------|-----------|
| 8 | 3.7V LiPo battery (1000 mAh) — solders to XIAO battery pads | $4 |
| 9 | 5V USB-C wall adapter (if stationary instead of battery) | $5 |
| 10 | 3D-printed or project box enclosure | $2–10 |
| | **Total (standalone device)** | **~$30–40** |

---

## Pin Usage Summary

| GPIO | XIAO Pad | Function | Component |
|------|----------|----------|-----------|
| 1    | D0       | I2S BCLK (mic) | INMP441 SCK |
| 2    | D1       | I2S WS (mic) | INMP441 WS |
| 3    | D2       | I2S DIN (mic) | INMP441 SD |
| 4    | D3       | Push-to-talk button | Momentary switch → GND |
| 9    | D10/SDA  | I2S BCLK (speaker) | MAX98357A BCLK |
| 10   | D7/SCL   | I2S WS (speaker) | MAX98357A LRC |
| 44   | D6       | I2S DOUT (speaker) | MAX98357A DIN |

Free GPIOs on XIAO (Wi-Fi mode): D4, D5, D8, D9 — available for LEDs, display, sensors, etc.
