# Unified Pico W SDR Firmware (v0.2)

High-performance, dual-mode SDR firmware for the **Intro-to-CAD-2026 v0.2 SDR Receiver** based on the **Raspberry Pi Pico W (RP2040 + CYW43439)**.

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

### Dual-Core Threading Model
- **Core 1 (Real-Time Audio DSP)**:
  - Samples the **PCM1808 24-bit ADC** via custom RP2040 PIO microcode (`i2s_rx.pio`) on GPIO 9, 10, 11.
  - Alternates ping-pong DMA buffers with zero CPU jitter.
  - Pushes 32-bit aligned I/Q frames into the lock-free inter-core hardware FIFO.
- **Core 0 (Networking & Protocol Engine)**:
  - Runs the **CYW43439 WiFi driver** and **lwIP** network stack.
  - Encapsulates stereo I/Q frames into **OpenHPSDR Protocol 1** frames (1032 bytes, EP6).
  - Packs stereo `S24_3LE` audio frames into the TinyUSB isochronous audio endpoint (UAC1).
  - Listens on USB CDC (`/dev/ttyACM0`) and TCP port 5000 for frequency and mode commands.
  - Computes the **Golden Integer LO algorithm on-chip in ~8 µs** upon receiving any `FREQ,<hz>` command.

---

## 2. OpenHPSDR Protocol 1 Subset Implementation

The firmware implements the standard **OpenHPSDR Protocol 1** over UDP Port `1024`, allowing automatic detection and streaming with SDR++, LinHPSDR, Thetis, SparkSDR, and Quisk.

### A. Discovery Broadcast (`0xEFFE 0x02`)
The host broadcasts a 60-byte UDP packet to port `1024` with header `0xEF 0xFE 0x02`. The Pico W replies with:
- `payload[0..1] = 0xEF 0xFE`
- `payload[2] = 0x02` (Discovery reply)
- `payload[3..8] = [Pico W WiFi MAC Address]`
- `payload[9] = 0x21` (Firmware version v3.3)
- `payload[10] = 0x06` (Board ID: Hermes / Metis compatible)

### B. Stream Control (`0xEFFE 0x04`)
- **Start Stream**: Host sends `0xEF 0xFE 0x04 0x01` -> Pico W begins transmitting 1032-byte I/Q frames to host IP/port.
- **Stop Stream**: Host sends `0xEF 0xFE 0x04 0x00` -> Pico W halts UDP transmission.

### C. In-Band Command & Control (C&C Header `0x7F 0x7F 0x7F`)
Embedded within the 512-byte subframe sync headers:
- **Command 0x01 (Tune Frequency)**: `[0x02, (freq>>24), (freq>>16), (freq>>8), freq]`. When received, Core 0 invokes the on-chip Si5351 synthesizer engine immediately.
- **Command 0x00 (Sample Rate)**: Speed bits `0x00` = 48,000 SPS, `0x01` = 96,000 SPS.

### D. I/Q Frame Structure (EP6: 1032 Bytes)
Each packet contains a 4-byte big-endian sequence number and **two 512-byte subframes** (126 total stereo samples):
```
Byte 0..1:   0xEF 0xFE (Frame Sync)
Byte 2:      0x01 (EP6 I/Q Data identifier)
Byte 3..6:   Sequence Number (32-bit big endian)
Byte 7:      Reserved (0x00)

[Subframe 1: 512 Bytes]
Byte 8..10:  Sync (0x7F 0x7F 0x7F)
Byte 11..15: C&C Command / Control data
Byte 16..519: 63 stereo samples (8 bytes each):
              [I2, I1, I0] (24-bit Left/I, MSB first)
              [Q2, Q1, Q0] (24-bit Right/Q, MSB first)
              [Mic1, Mic0] (16-bit microphone/reserved)

[Subframe 2: 512 Bytes]
Byte 520..522: Sync (0x7F 0x7F 0x7F)
Byte 523..527: C&C Command / Control data
Byte 528..1031: 63 stereo samples (same format as Subframe 1)
```

---

## 3. Hardware Pinout (Intro-to-CAD-2026 v0.2)

| Signal | Pico W GPIO | Function |
| :--- | :--- | :--- |
| **I2S DATA** | GPIO 9 | PCM1808 SDOUT (PIO Input) |
| **I2S BCK** | GPIO 10 | PCM1808 Bit Clock (PIO Input) |
| **I2S WS** | GPIO 11 | PCM1808 Word Select / LRCK (PIO Input) |
| **I2C SDA** | GPIO 12 | Si5351a Synthesizer I2C Data |
| **I2C SCL** | GPIO 13 | Si5351a Synthesizer I2C Clock |
| **MODE M0** | GPIO 22 | PCM1808 MD0 (Held LOW for I2S format) |
| **MODE M1** | GPIO 26 | PCM1808 MD1 (0 = 48 kHz, 1 = 96 kHz) |
| **Si5351 CLK2** | SCKI | 24.576 MHz Master Clock to PCM1808 |
| **Si5351 CLK0/1** | LO I/Q | Direct Quadrature LO to QSD Mixer |

---

## 4. Configuring WiFi Credentials (`wifi_config.h`)

The Raspberry Pi Pico W uses the on-board CYW43439 radio to connect to your local 2.4 GHz WiFi network in Station (STA) mode and stream UDP I/Q data via OpenHPSDR Protocol 1.

### Setting Your Network Credentials
Open `firmware/wifi_config.h` in your editor before compiling:

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

> **Note on Open Networks (No Password)**: If connecting to an open / unencrypted network, leave `DEFAULT_WIFI_PASSWORD` as `""`.
>
> **Dynamic Fallback**: If WiFi is disconnected or credentials are not yet configured, the Pico W **still operates fully via USB** (UAC1 24-bit audio device + USB CDC serial port `/dev/ttyACM0`).

---

## 5. How to Build & Flash `.uf2`

### Prerequisites (Ubuntu/Debian)
```bash
sudo apt update
sudo apt install -y gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib build-essential cmake git
```

### Build Commands
```bash
# 1. Clone TinyUSB if needed
if [ ! -d "$HOME/tinyusb" ]; then
    git clone https://github.com/hathach/tinyusb "$HOME/tinyusb"
fi

# 2. Edit WiFi credentials in firmware/wifi_config.h (see Section 4 above)

# 3. Build the UF2 binary
cd firmware
mkdir -p build && cd build
export PICO_SDK_PATH=/path/to/pico-sdk
export PICO_TINYUSB_PATH=$HOME/tinyusb

cmake -DPICO_BOARD=pico_w ..
make -j$(nproc)
```

Flash `sdr_pico_w_unified.uf2` by holding the **BOOTSEL** button on the Pico W while connecting the USB cable, then drag-and-drop the file into the `RPI-RP2` drive.

---

## 6. Verification Test Program (`test_pico_w_openhpsdr.py`)

A standalone Python verification script is included to test the network link without needing full SDR software open.

### Running the Test
```bash
# 1. Automatic network discovery and test:
python3 test_pico_w_openhpsdr.py

# 2. Or specify frequency and IP explicitly:
python3 test_pico_w_openhpsdr.py --ip 192.168.1.150 --freq 7050000 --rate 48000 --seconds 10
```

### Expected Output
```
[*] Sending OpenHPSDR discovery probe to 255.255.255.255:1024...
[+] Discovery Successful from 192.168.1.150:1024!
    - Board:    Hermes (Pico W)
    - MAC:      28:CD:C1:XX:XX:XX
    - Firmware: v3.3
[*] Sending C&C tune command: 7.050000 MHz (7050000 Hz, 48000 SPS)...
[*] Sending START streaming command (0xEFFE 0x04 0x01)...
[*] Streaming live 24-bit I/Q audio for 10.0 seconds...

  Elapsed |  Packets |  Lost | Rate (KB/s) |   Max Ampl | RMS (dBFS)
--------------------------------------------------------------------
    1.00s |      380 |     0 |     384.2 KB/s |     124800 |   -36.4 dB
    2.00s |      760 |     0 |     384.1 KB/s |     189200 |   -34.8 dB
    ...

==================================================
VERIFICATION TEST SUMMARY
==================================================
Target Pico W IP:      192.168.1.150
Tuned Frequency:       7.050000 MHz
Packets Received:      3800
Packets Dropped:       0 (0.00%)
Total Samples (I/Q):   478800
Throughput:            384.1 KB/s

[SUCCESS] OpenHPSDR Protocol 1 link on Pico W is verified and operational!
```

---

## 7. Using with Client Software

### SDR++ (WiFi)
1. Open **SDR++**.
2. Set **Source** to `OpenHPSDR`.
3. Click **Discover** (the Pico W appears).
4. Select `48000` or `96000` and click **Play**.

### Quisk (USB or WiFi)
- **USB Mode**: Set configuration file to `quisk_conf_unified.py` (captures ALSA `hw:S2026,0` and sends `FREQ,<hz>` commands over `/dev/ttyACM0`).
- **WiFi Mode**: Select the `OpenHPSDR` hardware module in Quisk.

---

## 8. Serial & TCP Command Reference

The firmware accepts ASCII commands over both the **USB CDC Serial Port** (`/dev/ttyACM0` at any baud rate) and the **Headless TCP Control Server** (Port 5000):

| Command | Response | Description |
| :--- | :--- | :--- |
| `WIFI?` | `WIFI,<STATUS>,IP:<ip>,SSID:<ssid>\r\nOK\r\n` | Queries WiFi link state (`CONNECTED`, `JOINING`, `NO_IP`, `DOWN`) and current IP |
| `WIFI,<ssid>,<pass>` | `WIFI,CONNECTING,<ssid>\r\nOK\r\n` | Dynamically updates WiFi credentials and initiates live connection |
| `FREQ,<hz>` | `<actual_hz>\r\nOK,<type>,<offset>\r\n` | Tunes LO using on-chip Golden Integer algorithm (e.g. `FREQ,7050000`) |
| `FREQ,` | `<last_hz>\r\nOK,<type>,0\r\n` | Queries the currently tuned LO frequency |
| `RATE,<48000\|96000>` | `RATE,<rate>\r\nOK\r\n` | Reconfigures PCM1808 hardware pins & PIO sample rate |
| `LOMODE,<BEST\|MID\|WORST\|FRAC>` | `OK\r\n` | Selects LO integer vs fractional spur-rejection tier |
| `VER` | `VER,SDR PCM1808 WiFi/USB 3.3\r\nOK\r\n` | Returns firmware identifier and version |
| `XTAL` | `XTAL,25000000\r\nOK\r\n` | Returns Si5351 reference crystal frequency (25 MHz) |
| `MODE` | `MODE,DIRECT\r\nOK\r\n` | Reports QSD direct quadrature vs Johnson counter mode |

