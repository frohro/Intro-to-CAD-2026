# Unified Pico W SDR Firmware (v0.2)

Unified firmware for the **Intro-to-CAD-2026 v0.2** SDR receiver board powered by the **Raspberry Pi Pico W** (RP2040 + CYW43439).

## Highlights

- **Dual-Mode Streaming**:
  - **USB Mode**: USB Audio Class 1.0 (UAC1) 24-bit stereo audio (`S24_3LE`) at **48 kHz** and **96 kHz** with USB CDC serial control.
  - **WiFi Mode**: **OpenHPSDR Protocol 1** over UDP (Port 1024) with standard auto-discovery, 24-bit I/Q network streaming, and TCP Control Server (Port 5000).
- **Autonomous On-Chip Si5351a Tuning**:
  - Performs **Golden Integer LO calculation directly on the RP2040** Cortex-M0+ core (~8 µs).
  - Maximizes spur-free dynamic range (SFDR) with fractional fallback for exact coverage.
- **Flexible Control Protocol**:
  - **Single Frequency**: `FREQ,<hz>` (e.g. `FREQ,7050000`) computes optimal integer parameters on the Pico W.
  - **Legacy Quisk Mode**: `FREQ,<hz>,<N>,<a>,<b>,<c>,<P1>,<P2>,<P3>` maintains 100% backward compatibility with existing lab scripts.
  - **LO Optimization Mode**: `LOMODE,BEST`, `LOMODE,MID`, `LOMODE,WORST`, `LOMODE,FRAC`.
- **Dynamic Sample Rate Switching**:
  - Controlled dynamically via CDC/TCP (`RATE,48000` / `RATE,96000`) or OpenHPSDR C&C packets.

## Pinout (v0.2 Board)

| Signal | GPIO | Function |
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

## WiFi Configuration

Edit `wifi_config.h`:
```c
#define DEFAULT_WIFI_SSID       "YourWiFiNetwork"
#define DEFAULT_WIFI_PASSWORD   "YourPassword"
```

## How to Build the `.uf2` Firmware

```bash
# 1. Clone TinyUSB if needed
git clone https://github.com/hathach/tinyusb ~/tinyusb
export PICO_TINYUSB_PATH=$HOME/tinyusb

# 2. Build for Pico W
mkdir -p build && cd build
cmake -DPICO_BOARD=pico_w ..
make -j4
```

Flash `sdr_pico_w_unified.uf2` to your Pico W via bootsel mode.

## Using with Linux SDR Software

### 1. SDR++ / LinHPSDR / Thetis (WiFi Mode)
1. Ensure your PC is on the same WiFi network as the Pico W.
2. In SDR++, set Source to **OpenHPSDR**.
3. Click **Discover** (or enter the Pico W's IP).
4. Select 48 kHz or 96 kHz and click Play.

### 2. Quisk (USB or WiFi Mode)
- **USB**: Use `quisk_conf_unified.py` — captures ALSA `hw:S2026,0` and controls via `/dev/ttyACM0`.
- **WiFi**: Select the OpenHPSDR hardware profile in Quisk.
