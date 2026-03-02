# PlatformIO to Native ESP-IDF Migration Guide

This document details the exact steps taken to migrate the `ESP32-S3-XIAO` cellular modem simulator project from PlatformIO to a native ESP-IDF project structure with Wokwi support.

## 1. Source Code Migration

The biggest difference between PlatformIO and ESP-IDF is how source files are structured. PlatformIO defaults to a root `src/` directory, while ESP-IDF uses a component-based structure where the main application lives in a `main/` component.

**Steps taken:**
1. Copied all `.c` and `.h` files from `ESP32-S3-XIAO/src/` to `ESP32-S3-XIAO_ESPIDF/sample_project/main/`.
   - `main.c` (The standalone entry point)
   - `sim_modem.c` and `sim_modem.h`
   - `modem_driver.c` and `modem_driver.h`

The code itself required **zero changes** because it was already written using standard ESP-IDF APIs (FreeRTOS, UART, esp_netif) and did not rely on PlatformIO-specific macros.

## 2. Build System Configuration (`CMakeLists.txt`)

PlatformIO hides CMake complexity, often automatically globbing all files in `src/`. Native ESP-IDF requires explicit component registration.

**Steps taken:**
1. Updated `ESP32-S3-XIAO_ESPIDF/sample_project/main/CMakeLists.txt` to explicitly list the source files and the ESP-IDF components they depend on:

```cmake
idf_component_register(SRCS "main.c" "sim_modem.c" "modem_driver.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_netif esp_event driver)
```
*Note: We explicitly added `REQUIRES esp_netif esp_event driver` because `main.c` and the modem files rely on these components for networking and UART control.*

## 3. Project Configuration (`sdkconfig`)

PlatformIO generates a massive `sdkconfig` file (often 2000+ lines) containing every possible configuration flag. ESP-IDF uses a different, cleaner approach: you provide a small `sdkconfig.defaults` file containing only your overrides, and ESP-IDF generates the full config during the build process.

**Steps taken:**
1. Copied `ESP32-S3-XIAO/sdkconfig.defaults` to `ESP32-S3-XIAO_ESPIDF/sample_project/sdkconfig.defaults`.
2. This file ensures that critical project settings are applied when you run `idf.py set-target esp32s3`:
   - PPP support (`CONFIG_PPP_SUPPORT=y`)
   - 8MB Flash size (`CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y`)
   - CPU Frequency at 160MHz (`CONFIG_ESP32S3_DEFAULT_CPU_FREQ_160=y`)
   - Console UART baud rate (`CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200`)

## 4. Wokwi Simulator Setup

Wokwi works great with both PlatformIO and ESP-IDF, but the paths to the generated `.elf` and `.bin` files change depending on the build system.

**Steps taken:**
1. Copied the hardware layout file `diagram.json` from the old project to the new project root.
2. Created a new `wokwi.toml` file in `ESP32-S3-XIAO_ESPIDF/sample_project/` and updated the paths to point to the ESP-IDF `build/` directory instead of PlatformIO's `.pio/build/` directory:

```toml
[wokwi]
version = 1
elf = "build/sample_project.elf"
firmware = "build/sample_project.bin"
```

## 5. Documentation

**Steps taken:**
1. Copied all informational Markdown files (`README.md`, `DESIGN.md`, `SIM_MODEM_FSM.md`) from the PlatformIO project to the new ESP-IDF project.

---

## 6. Local Environment Setup Guide

Follow these exact steps to set up your local development environment to match your previous setup (with FreeRTOS, ESP-IDF, and Wokwi) and get the project running.

### Step 1: Install Prerequisites
1. Install [Visual Studio Code](https://code.visualstudio.com/) if you haven't already.
2. Install [Python](https://www.python.org/downloads/) (Make sure to check the box that says **"Add Python to PATH"** during installation).
3. Install [Git](https://git-scm.com/downloads) (Required for downloading ESP-IDF components).

### Step 2: Install and Configure ESP-IDF
1. Open VS Code.
2. Go to the Extensions view (`Ctrl+Shift+X` or `Cmd+Shift+X`).
3. Search for **"ESP-IDF"** and install the official extension by **Espressif Systems**.
4. Open the Command Palette (`Ctrl+Shift+P` or `Cmd+Shift+P`), type **"ESP-IDF: Configure ESP-IDF extension"** and select it.
5. Choose the **"Express"** setup option.
6. Select an ESP-IDF version (v5.x or newer is recommended for ESP32-S3 support) and specify the installation directories.
7. Click **"Install"**. This will download the ESP-IDF framework (which includes FreeRTOS, networking libraries, driver modules, etc.), the cross-compiler toolchain, and set up the Python virtual environment. Wait for it to show a success message.

### Step 3: Install Wokwi for Simulation
1. Go back to the Extensions view in VS Code.
2. Search for **"Wokwi Simulator"** and install it.
3. Open the Command Palette (`Ctrl+Shift+P`), type **"Wokwi: Request a New License"** to get a free license, and follow the browser prompts to activate it.

### Step 4: Open and Configure the Project
1. In VS Code, go to **File > Open Folder...** and select the `ESP32-S3-XIAO_ESPIDF/sample_project` directory.
2. *Important:* The ESP-IDF extension needs to know your target chip. Open the Command Palette (`Ctrl+Shift+P`) and type **"ESP-IDF: Set Espressif device target"**.
3. Select **"esp32s3"**.
4. This command will read your `sdkconfig.defaults` and automatically generate the massive, fully fleshed-out `sdkconfig` file for your project (incorporating FreeRTOS and all enabled modules).

### Step 5: Build the Project
1. Look at the bottom ESP-IDF status bar in VS Code and click the **"Build Project"** icon (it looks like a little cylinder), OR open the Command Palette and type **"ESP-IDF: Build your project"**.
2. Wait for the compilation to complete. This process builds `main.c`, `sim_modem.c`, `modem_driver.c`, and all the necessary FreeRTOS and networking components. 
3. You will see a success message, and the compiled artifacts will be placed in the `build/` directory (specifically `build/sample_project.elf` and `build/sample_project.bin`).

### Step 6: Run the Simulator
1. Open the `diagram.json` file in VS Code.
2. In the top right corner of the editor (or as a hover button over the diagram code), click the **Play button (▶️)** to start the Wokwi simulator.
3. Because we configured `wokwi.toml` to point directly to the new ESP-IDF `build/` folder, Wokwi will automatically flash your newly compiled firmware into the virtual ESP32-S3 XIAO.
4. The integrated serial monitor will open at the bottom of the screen, where you can watch the FreeRTOS boot logs and your application running natively!