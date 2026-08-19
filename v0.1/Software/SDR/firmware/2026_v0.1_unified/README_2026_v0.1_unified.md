# Unified Pico W SDR Firmware (v0.1 Hardware)

High-performance, dual-mode SDR firmware for the **Intro-to-CAD-2026 v0.1 SDR Receiver Board** based on the **Raspberry Pi Pico W (RP2040 + CYW43439)**.

---

## 1. System Architecture

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
| Si5351a Synth   |<---|                                               |             |     (SDR++, Thetis, Quisk)
| (I2C QSD LO)    |----+-----------------------------------------------+             |
+-----------------+                                                                  |
                       +-------------------------------------------------------------+
```

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

---

## 3. How to Build & Flash `.uf2`

```bash
mkdir -p build && cd build
export PICO_SDK_PATH=/path/to/pico-sdk
export PICO_TINYUSB_PATH=$HOME/tinyusb

cmake -DPICO_BOARD=pico_w ..
make -j$(nproc)
```

Flash `sdr_pico_w_v0.1_unified.uf2` by holding the **BOOTSEL** button on the Pico W while connecting the USB cable, then drag-and-drop the file into the `RPI-RP2` drive.
