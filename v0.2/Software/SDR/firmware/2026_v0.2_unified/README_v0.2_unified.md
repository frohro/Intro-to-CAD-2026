# Intro-to-CAD-2026 v0.2 Unified Pico W SDR Receiver

**Repository**: `Intro-to-CAD-2026`  
**Target Hardware**: Raspberry Pi Pico W (RP2040 + CYW43439), PCM1808 24-bit I2S Stereo ADC, Si5351A Clock Generator / Quadrature LO Synthesizer, Tayloe QSD Mixer.

---

## 1. Project Overview

The **Intro-to-CAD-2026 v0.2 Unified Firmware** is a high-performance, dual-mode Software Defined Radio (SDR) receiver firmware and companion software suite. It supports simultaneous or selectable operation over **High-Speed USB** and **2.4 GHz 802.11n Wi-Fi**:

1. **Wi-Fi Mode (OpenHPSDR Protocol 1 / Hermes)**:
   - Connects to your local Wi-Fi network in Station (STA) mode.
   - Streams 24-bit stereo I/Q audio via standard **OpenHPSDR Protocol 1** over UDP port `1024` at **48,000 SPS** or **96,000 SPS** (1032-byte EP6 frames).
   - Compatible out-of-the-box with **Quisk**, **SDR++**, **Thetis**, **LinHPSDR**, **SparkSDR**, and **GNU Radio**.
   - Supports network Discovery (`0xEFFE 0x02`), Run/Stop (`0xEFFE 0x04`), and In-Band Command & Control (C&C) for RX 0 frequency tuning (Command `0x02`), VFO tuning (Command `0x01`), and sample rate switching (Command `0x00`).
   - Dedicated TCP headless command server on port `5000`.

2. **USB Mode (UAC1 24-bit Audio + CDC Serial Control)**:
   - Enumerates as a standard USB Audio Class 1.0 (UAC1) 24-bit stereo capture device (`S24_3LE`, 48 kHz / 96 kHz).
   - Exposes a USB CDC serial port (`/dev/ttyACM0` on Linux, `COMx` on Windows) for interactive frequency tuning (`FREQ,<hz>`), sample rate control (`RATE,<48000|96000>`), and hardware diagnostics.

---

## 2. System Architecture

```
                       +-------------------------------------------------------------+
                       |                    Raspberry Pi Pico W                      |
                       |                                                             |
+-----------------+    |   +-------------------+              +------------------+   |    USB Type-C
| PCM1808 ADC     |--->|   | Core 1 (Audio VM) |              | Core 0 (Network) |   |===> UAC1 (24-bit 48/96k)
| (I2S Stereo 24b)|    |   | - PIO State Mach  |  FIFO Queue  | - lwIP / CYW43   |   |     USB CDC Control
+-----------------+    |   | - Ping-Pong DMA   |------------->| - TinyUSB UAC1   |   |
                       |   | - Dynamic 48k/96k |              | - On-Chip Si5351 |   |    WiFi (UDP 1024)
+-----------------+    |   +-------------------+              +------------------+   |~~~> OpenHPSDR Protocol 1
| Si5351a Synth   |<---|                                               |             |     (SDR++, Quisk, Thetis)
| (I2C QSD LO)    |----+-----------------------------------------------+             |
+-----------------+                                                                  |
                       +-------------------------------------------------------------+
```

### Dual-Core Allocation
- **Core 1 (Real-Time Audio DSP Pipeline)**:
  - Samples the PCM1808 24-bit ADC via custom RP2040 PIO microcode (`i2s_rx.pio`) on GPIO 9, 10, and 11.
  - Alternates ping-pong DMA buffers with zero CPU jitter.
  - Pushes 32-bit aligned I/Q frames into the lock-free inter-core hardware FIFO.
- **Core 0 (Networking & Protocol Engine)**:
  - Runs the CYW43439 Wi-Fi driver and lwIP network stack in non-blocking event-driven mode.
  - Encapsulates stereo I/Q frames into OpenHPSDR Protocol 1 frames (1032 bytes, EP6).
  - Packs stereo `S24_3LE` audio frames into the TinyUSB isochronous audio endpoint (UAC1).
  - Listens on USB CDC (`/dev/ttyACM0`) and TCP port 5000 for frequency and mode commands.
  - Computes the Golden Integer LO synthesizer algorithm on-chip in ~8 µs upon receiving any frequency command.

---

## 3. Hardware Pinout (Intro-to-CAD-2026 v0.2)

| Signal | Pico W GPIO | Function | Description |
| :--- | :--- | :--- | :--- |
| **I2S DATA** | GPIO 9 | PCM1808 SDOUT (PIO RX) | 24-bit I2S audio stream from ADC |
| **I2S BCK** | GPIO 10 | PCM1808 Bit Clock (PIO Input) | 64 × $F_s$ I2S Bit Clock |
| **I2S WS** | GPIO 11 | PCM1808 Word Select (PIO Input) | $F_s$ Left/Right Frame Clock |
| **I2C SDA** | GPIO 12 | Si5351A I2C Data | Frequency synthesizer control |
| **I2C SCL** | GPIO 13 | Si5351A I2C Clock | Frequency synthesizer control |
| **MODE M0** | GPIO 22 | PCM1808 MD0 | Held LOW for standard I2S format |
| **MODE M1** | GPIO 26 | PCM1808 MD1 | 0 = 48 kHz (256 $F_s$), 1 = 96 kHz (256 $F_s$) |
| **Si5351 CLK2** | SCKI Pin | Master Clock (24.576 MHz) | Master clock fed to PCM1808 ADC |
| **Si5351 CLK0/1** | LO I/Q | Direct Quadrature LO | 0° / 90° LO clocks to QSD mixer |

---

## 4. Directory & File Structure

```
2026_v0.2_unified/
├── CMakeLists.txt              # Pico SDK CMake build configuration
├── pico_sdk_import.cmake       # Pico SDK import helper
├── lwipopts.h                  # lwIP TCP/IP stack configuration for Pico W
├── tusb_config.h               # TinyUSB configuration (UAC1 + CDC)
├── wifi_config.h               # Default Wi-Fi SSID, password, and port settings
├── i2s_rx.pio                  # RP2040 PIO microcode for 24-bit I2S stereo capture
├── main.c                      # Dual-core firmware entry point & system orchestration
├── openhpsdr.c / .h            # OpenHPSDR Protocol 1 network engine (UDP 1024)
├── si5351.c / .h               # Si5351A synthesizer driver & Golden Integer LO engine
├── usb_descriptors.c           # USB descriptors (UAC1 24-bit stereo audio + CDC serial)
├── test_pico_w_openhpsdr.py    # Standalone command-line Wi-Fi OpenHPSDR tester & benchmark
├── debug_hpsdr_tap.py          # Real-time UDP network traffic & C&C packet sniffer
├── quisk_conf_openhpsdr.py     # Quisk configuration for Wi-Fi OpenHPSDR operation
├── quisk_conf_unified.py       # Quisk configuration for USB UAC1 + CDC operation
├── quisk_widgets.py            # Custom Quisk toolbar widgets & band-switch buttons
├── quisk_widgets_picow.py      # Extended Quisk toolbar widgets with status display
└── README_v0.2_unified.md      # Comprehensive technical documentation & user guide
```

---

## 5. Wi-Fi Configuration (`wifi_config.h`)

Edit `wifi_config.h` before building if you want to hardcode your network:

```c
#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#define DEFAULT_WIFI_SSID       "YourNetworkSSID"
#define DEFAULT_WIFI_PASSWORD   "YourPassword"   // Leave as "" for open networks
#define TCP_CONTROL_PORT        5000

#endif // WIFI_CONFIG_H
```

> **Dynamic Configuration**: Wi-Fi credentials can also be changed dynamically without reflashing by sending `WIFI,<ssid>,<pass>` over the USB CDC serial terminal (`/dev/ttyACM0`) or TCP port 5000.

---

## 6. How to Build and Flash

### Build Requirements
- Raspberry Pi Pico SDK (v1.5.0 or later)
- ARM GNU Toolchain (`arm-none-eabi-gcc`, `arm-none-eabi-newlib`)
- CMake (>= 3.13) & Make
- Hathach TinyUSB submodule

### Compilation
```bash
# Set environment paths
export PICO_SDK_PATH=/path/to/pico-sdk
export PICO_TINYUSB_PATH=/path/to/tinyusb

# Build UF2 binary
mkdir -p build && cd build
cmake -DPICO_BOARD=pico_w ..
make -j$(nproc)
```

### Flashing
1. Hold down the **BOOTSEL** button on the Pico W while plugging it into USB.
2. The board will mount as a mass-storage drive named `RPI-RP2`.
3. Copy `pico_w_sdr.uf2` into `RPI-RP2`.
4. The Pico W will reboot automatically and begin operation.

---

## 7. Client Software Setup & Quick Start

### A. Wi-Fi Mode with Quisk
Run Quisk using the included OpenHPSDR configuration:
```bash
quisk -c quisk_conf_openhpsdr.py
```
- In Quisk, the radio will connect automatically to the Pico W IP address.
- Click the ham band buttons on the custom toolbar to tune directly to 160m, 80m, 40m, 30m, 20m, 17m, 15m, 12m, or 10m.

### B. USB Mode with Quisk
Run Quisk using the unified USB configuration:
```bash
quisk -c quisk_conf_unified.py
```
- Quisk captures 24-bit audio directly from the Pico W ALSA sound card (`hw:S2026,0`) and controls the hardware LO over USB CDC (`/dev/ttyACM0`).

### C. Wi-Fi Mode with SDR++
1. Open **SDR++**.
2. Select **Source** -> `OpenHPSDR / Hermes`.
3. Click **Discover** (the Pico W will appear with its IP address and Hermes board ID).
4. Select `48000` or `96000` sample rate and click **Play**.

### D. Standalone Command-Line Verification (`test_pico_w_openhpsdr.py`)
To test network discovery, LO tuning, packet reception, throughput, and RMS signal levels without opening full SDR software:
```bash
python3 test_pico_w_openhpsdr.py --ip 192.168.1.186 --freq 7050000 --rate 48000 --seconds 10
```

---

## 8. Serial & TCP Command Reference

Commands can be sent via USB CDC (`/dev/ttyACM0`) or TCP port `5000`:

| Command | Response | Description |
| :--- | :--- | :--- |
| `FREQ,<hz>` | `<actual_hz>\r\nOK,DIRECT,0\r\n` | Tunes Si5351 LO (e.g. `FREQ,7050000`) |
| `FREQ,` | `<last_hz>\r\nOK,DIRECT,0\r\n` | Returns current tuned frequency in Hz |
| `RATE,<48000\|96000>` | `RATE,<rate>\r\nOK\r\n` | Switches PCM1808 hardware pins & PIO sample rate |
| `WIFI?` | `WIFI,<STATUS>,IP:<ip>,SSID:<ssid>\r\nOK\r\n` | Queries Wi-Fi connection status and IP address |
| `WIFI,<ssid>,<pass>` | `WIFI,CONNECTING,<ssid>\r\nOK\r\n` | Connects dynamically to specified Wi-Fi network |
| `VER` | `VER,SDR PCM1808 WiFi/USB 3.3\r\nOK\r\n` | Returns firmware identifier and version |
| `XTAL` | `XTAL,24576000\r\nOK\r\n` | Returns nominal Si5351 master crystal frequency |
| `MODE` | `MODE,DIRECT\r\nOK\r\n` | Reports QSD direct quadrature mode |
