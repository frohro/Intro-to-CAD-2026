# Research: 48/96/192 kHz Multi-ADC UAC1 SDR (v0.2)

This experimental firmware for the **Intro-to-CAD-2026 v0.2** board (YD-RP2040) enables runtime switching between two different ADCs and multiple sample rates without changing physical jumpers.

## Key Features
- **Dual ADC Support**: Supports both the **PCM1808** and the **CJC5430** (CJC5430) on the same board.
- **Dynamic Rate Switching**: Supports 48 kHz, 96 kHz, and 192 kHz (CJC5430 only).
- **USB Audio Class 1.0**:
    - **48 kHz / 96 kHz**: 24-bit stereo (S24_3LE) to maximize dynamic range.
    - **192 kHz**: 16-bit stereo (S16_LE) with TPDF dither to fit within USB Full Speed bandwidth.
- **Software Mode Control**: Replaces physical jumpers M0, M1, and MD1 with GPIO control.

## Hardware Configuration (v0.2 Board)
The following GPIOs are used for mode selection and I2S interfacing:

### Mode Selection GPIOs
| Signal | GPIO | Logic Level (PCM1808) | Logic Level (CJC5430) |
| :--- | :--- | :--- | :--- |
| **M0** | 22 | MD0 (0=I2S) | M0 (0=48k/192k, 1=96k) |
| **M1/MD1** | 26 | MD1 (0=48k, 1=96k) | M1 (0=48k/96k, 1=192k) |

### ADC-Specific Pinouts
| Signal | PCM1808 Pin | CJC5430 Pin |
| :--- | :--- | :--- |
| **DATA** | GPIO 9 | GPIO 6 |
| **BCK** | GPIO 10 | GPIO 7 |
| **WS** | GPIO 11 | GPIO 8 |
| **FMT** | GPIO 6 (Fixed LOW) | N/A |

### Shared Pins
- **I2C**: SDA=GPIO 12, SCL=GPIO 13 (Si5351a)

## CDC Control Protocol
Quisk controls the hardware using an enhanced line-oriented ASCII protocol over the CDC port:

- `ADC,PCM1808` : Select the PCM1808 ADC path.
- `ADC,CJC5430` : Select the CJC5430 ADC path.
- `RATE,<hz>` : Set pending sample rate (48000, 96000, or 192000).
- `FREQ,<hz>,<N>,<a>,<b>,<c>,<P1>,<P2>,<P3>` : Program the Si5351a LO.

## USB Alternate Settings
The device advertises one Audio Streaming interface with multiple alternate settings:
1. **Alt 1**: 48,000 Hz, 2ch, 24-bit (S24_3LE)
2. **Alt 2**: 96,000 Hz, 2ch, 24-bit (S24_3LE)
3. **Alt 3**: 192,000 Hz, 2ch, 16-bit (S16_LE)

## Building the Firmware
This project requires a standalone **TinyUSB** checkout (version >= 0.19) as the version bundled with the Pico SDK does not include the necessary UAC1 headers.

```bash
# 1. Clone TinyUSB if you haven't already
git clone https://github.com/hathach/tinyusb ~/tinyusb
export PICO_TINYUSB_PATH=$HOME/tinyusb

# 2. Build the project
mkdir build && cd build
cmake ..
make
```

Flash the resulting `sdr_2026_v0_2.uf2` to your YD-RP2040.
