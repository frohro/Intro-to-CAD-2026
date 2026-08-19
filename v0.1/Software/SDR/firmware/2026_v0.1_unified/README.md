# Unified Pico W / Pico 2 W SDR Firmware (v0.1 Hardware)

High-performance, dual-mode SDR firmware for the **Intro-to-CAD-2026 v0.1 SDR Receiver Board** targeting either the **Raspberry Pi Pico W (RP2040 + CYW43439)** or **Raspberry Pi Pico 2 W (RP2350 + CYW43439)**.

---

## 1. System Architecture

```
                       +-------------------------------------------------------------+
                       |              Raspberry Pi Pico W / Pico 2 W                 |
                       |                                                             |
+-----------------+    |   +-------------------+              +------------------+   |    USB Type-C
| PCM1808 ADC     |--->|   | Core 1 (Audio VM) |              | Core 0 (Network) |   |===> UAC1 (24-bit 48/96k)
| (I2S Stereo 24b)|    |   | - PIO State Mach  |  FIFO Queue  | - lwIP / CYW43   |   |     USB CDC Control
+-----------------+    |   | - Ping-Pong DMA   |------------->| - TinyUSB UAC1   |   |
                       |   | - Dynamic 48k/96k |              | - On-Chip Si5351 |   |    WiFi (UDP 1024)
+-----------------+    |   +-------------------+              +------------------+   |~~~> OpenHPSDR Protocol 1
| Si5351a Synth   |<---|                                               |             |     (SDR++, Thetis, Quisk)
| (I2C QSD LO)    |----+-----------------------------------------------+             |
+-----------------+                                                                  |
                       +-------------------------------------------------------------+
```

### Dual-Core Threading Model
- **Core 1 (Real-Time Audio DSP)**:
  - Samples the **PCM1808 24-bit ADC** via custom RP2040/RP2350 PIO microcode (`i2s_rx.pio`) on **GPIO 14, 15, 16**.
  - Alternates ping-pong DMA buffers with zero CPU jitter.
  - Pushes 32-bit aligned I/Q frames into the lock-free inter-core hardware FIFO.
- **Core 0 (Networking & Protocol Engine)**:
  - Runs the **CYW43439 WiFi driver** and **lwIP** network stack.
  - Encapsulates stereo I/Q frames into **OpenHPSDR Protocol 1** frames (1032 bytes, EP6).
  - Packs stereo `S24_3LE` audio frames into the TinyUSB isochronous audio endpoint (UAC1).
  - Listens on USB CDC (`/dev/ttyACM0`) and TCP port 5000 for frequency and mode commands.
  - Computes the **Golden Integer LO algorithm on-chip in ~8 µs** upon receiving any `FREQ,<hz>` command.

---

## 2. GPIO Pin Assignments (Intro-to-CAD-2026 v0.1 Board)

All assignments target the **Intro-to-CAD-2026 v0.1 PCB**:

| GPIO | Signal | Direction | Notes |
| :--- | :--- | :--- | :--- |
| **10** | **FMT** | OUT (LOW) | PCM1808 FMT — LOW = I2S standard format |
| **11** | **MD1** | OUT | PCM1808 MD1 — LOW = 512fs / 48 kHz, HIGH = 256fs / 96 kHz |
| **12** | **SDA** | I2C | Si5351a / MS5351M I2C data (100 kHz) |
| **13** | **SCL** | I2C | Si5351a / MS5351M I2C clock |
| **14** | **DATA** | IN (PIO) | PCM1808 DOUT — I2S serial audio |
| **15** | **BCK** | IN (PIO) | PCM1808 BCK — bit clock from ADC |
| **16** | **WS** | IN (PIO) | PCM1808 LRCK — word select / frame sync |
| **22** | **DBG** | OUT | DMA ISR toggle — ~500 Hz square wave for logic analyser |

*Note on BCK*: BCK at 48 kHz = 3.072 MHz; at 96 kHz = 6.144 MHz. PIO runs at 125 MHz (32 ns/cycle) leaving ~5× margin at the fastest BCK rate.

---

## 3. How to Build & Flash `.uf2`

### Option A: Using the Build Script

```bash
# To build for Raspberry Pi Pico W (RP2040):
chmod +x build.sh
./build.sh pico_w

# To build for Raspberry Pi Pico 2 W (RP2350):
./build.sh pico2_w
```

### Option B: Using CMake Directly

```bash
# For Pico W (RP2040):
mkdir -p build_pico_w && cd build_pico_w
cmake -DPICO_BOARD=pico_w ..
make -j$(nproc)

# For Pico 2 W (RP2350):
mkdir -p build_pico2_w && cd build_pico2_w
cmake -DPICO_BOARD=pico2_w ..
make -j$(nproc)
```

Flash the generated `.uf2` binary by holding the **BOOTSEL** button on the Pico W / Pico 2 W while connecting the USB cable, then drag-and-drop the file into the `RPI-RP2` (or `RP2350`) drive.

---

## 4. Configuring WiFi Credentials (`wifi_config.h`)

Open `wifi_config.h` before compiling:

```c
#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

// Replace with your 2.4 GHz WiFi network name and password:
#define DEFAULT_WIFI_SSID       "YourWiFiNetworkName"
#define DEFAULT_WIFI_PASSWORD   "YourWiFiPassword"

// Optional TCP headless control port (default: 5000)
#define TCP_CONTROL_PORT        5000

#endif // WIFI_CONFIG_H
```

> **Dynamic Fallback**: If WiFi is disconnected or credentials are not yet configured, the Pico W / Pico 2 W **still operates fully via USB** (UAC1 24-bit audio device + USB CDC serial port `/dev/ttyACM0`).

---

## 5. Verification & Client Software

- **Test Script**: Run `python3 test_pico_w_openhpsdr.py` to verify OpenHPSDR streaming and packet health.
- **SDR++**: Set Source to `OpenHPSDR`, discover the board, and press Play.
- **Quisk**:
  - USB Mode: `quisk -c quisk_conf_unified.py -v`
  - WiFi Mode: `quisk -c quisk_conf_openhpsdr.py -v`
