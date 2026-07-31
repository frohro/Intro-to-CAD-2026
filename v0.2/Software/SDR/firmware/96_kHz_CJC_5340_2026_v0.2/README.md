# WWU SDR — 96 kHz Fixed-Rate CJC5340 Firmware (v0.2)

**Tested hardware:** Intro-to-CAD-2026 v0.2 board (YD-RP2040 module)

> **Note:** This firmware targets the **v0.2 board** and is hardcoded for **96 kHz**.
> It uses **Johnson counter mode** (clk0 -> I/Q clock) by default.
> `BOARD_DIRECT_MODE` is set to `false`.

RP2040 firmware that captures stereo I2S audio from a CJC5340 ADC and
streams it to a Linux host as a UAC1 (USB Audio Class 1) device at 
96 kHz in S24\_3LE format.  A Si5351a clock synthesiser provides
the quadrature local-oscillator signal for the SDR front end.  A CDC
interface carries the Quisk tune protocol.

---

## Hardware Requirements

| Part | Description |
|------|-------------|
| YD-RP2040 | RP2040 development board (USB-C, 16 MB flash) |
| CJC5340 | 24-bit stereo ADC, I2S master mode (CS5340 compatible) |
| Si5351a | Clock synthesiser (I2C address 0x60, A0 pin pulled low) |
| 24.576 MHz TCXO/crystal | Reference — 256 × 96 000 = 24 576 000 Hz |

---

## GPIO Pin Assignments

All assignments are for the **Intro-to-CAD-2026 v0.2 board** (CJC5340 + I2C).

| GPIO | Signal | Direction | Notes |
|------|--------|-----------|-------|
| JP_M0 | M0 | - | ADC Jumper — MUST BE HIGH for 96 kHz (256fs) |
| JP_M1 | M1 | - | ADC Jumper — MUST BE LOW for Single/Double Speed |
| 12 | SDA | I2C | Si5351a I2C data (100 kHz) |
| 13 | SCL | I2C | Si5351a I2C clock |
| 6  | DATA | IN (PIO) | CJC5340 SDOUT — I2S serial audio |
| 7  | BCK  | IN (PIO) | CJC5340 SCLK — bit clock from ADC |
| 8  | WS   | IN (PIO) | CJC5340 LRCK — word select / frame sync |
| -  | /RST | - | CJC5340 /RST — Wired HIGH on v0.2 |
| 22 | DBG  | OUT | DMA ISR toggle (logic analyser probe) |

---

## Si5351a Clock Usage (v0.2 board)

| Clock | Signal | Frequency | Notes |
|-------|--------|-----------|-------|
| CLK0 | SCKI | 24.576 MHz | ADC Master Clock (crystal bypass) |
| CLK2 | LO_IN | 4 × TuneFreq | Input to Johnson Counter for I/Q |

BCK at 96 kHz = 6.144 MHz.  PIO runs at 125 MHz
(32 ns/cycle) leaving ~5× margin.
---

## USB Audio Format — S24\_3LE

This section describes the complete data path from ADC bits to USB bytes so
that byte order, justification, and sign convention can be verified
independently.

### CJC5340 output word format (I2S Standard)

The CJC5340 drives SCLK and LRCK as master outputs.  In I2S standard format
data is MSB-first, delayed one SCLK from the LRCK edge, with 24 valid bits
followed by 8 trailing zero-pad bits per channel:

```
SCLK edge:  1   2   3  ...  24  25  26 ... 32
           -   D23 D22 ...  D1  D0   0 ...  0
               ^sign/MSB        ^LSB  <-zero pad->
```

### PIO capture → 32-bit DMA word

`i2s_rx.pio` shifts DATA in on every SCLK rising edge, MSB first, and
autopushes after 32 bits.  Because of the 1-bit I2S delay, the first bit
captured is a dummy bit (usually 0). The resulting word in `buf_a[]` / `buf_b[]` is:

```
Bit:  31     30  29  28 ...  7   6   5 ... 0
      dummy  D23 D22 D21 ... D0   0   0 ... 0
             ^sign/MSB       ^LSB  <-zeros->
```

The sample is effectively **left-justified** but shifted right by one bit.

### audio\_task() packing → 3 USB bytes

`audio_task()` corrects the 1-bit shift and packs each 32-bit DMA word to three bytes:

```c
uint32_t w = src[i] << 1;     // Align D23 to bit 31
byte 0 = (w >>  8) & 0xFF     // D7..D0  — LSB
byte 1 = (w >> 16) & 0xFF     // D15..D8
byte 2 = (w >> 24) & 0xFF     // D23..D16 — MSB, sign bit in bit 7
```

Bytes are emitted LSB-first.  The zero-pad byte (bits 7:0 of the DMA word)
is discarded.  The result is `SNDRV_PCM_FORMAT_S24_3LE`: little-endian
signed 24-bit audio packed as 3 bytes per sample.

### Sign extension

D23 (the two's-complement sign bit) is in bit 31 of the DMA word.  After
`>> 24` it sits in bit 7 of byte 2 — the MSB of the three-byte group.
`snd-usb-audio` reads the three bytes and sign-extends from bit 23 to
produce a correct signed 32-bit integer.  No firmware intervention is needed.

### USB descriptor

The device exposes **one active setting** (96 kHz) on Interface 3:

| Alt | Rate | wMaxPacketSize | Notes |
|-----|------|---------------|-------|
| 0 | — | 0 | Zero-bandwidth (idle) |
| 1 | 96 000 Hz | 582 B | 576 B/ms + 6 B headroom (±1 sample) |

```
bSubframeSize  = 3    (3 bytes per sample)
bBitResolution = 24   (24 significant bits)
bSamFreqType   = 1    (one discrete frequency per alt setting)
```

`snd-usb-audio` automatically selects alt 1 for the 96 kHz stream.

### TinyUSB internal buffer sizing (`tusb_config.h`)

`CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX` is set to **582 bytes** and
`CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ` to **2352 bytes**.  

---

## Build Instructions

### Prerequisites

1. Install the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk)
   and set `PICO_SDK_PATH`:
   ```bash
   export PICO_SDK_PATH=$HOME/pico-sdk
   ```

2. Clone the TinyUSB **master branch** (the pico-sdk bundled version does not
   include UAC1 support):
   ```bash
   git clone https://github.com/hathach/tinyusb $HOME/tinyusb
   export PICO_TINYUSB_PATH=$HOME/tinyusb
   ```

### Compile

```bash
mkdir build
cd build
cmake ..
make -j4
```

Flash `sdr_96k.uf2` to the Pico.

---

## Operation

### Linux Host Verification (ALSA)

Check that the device is detected as "WWU SDR" with S24\_3LE support:

```bash
arecord -l | grep "WWU SDR"
# card 2: SDR [WWU SDR], device 0: USB Audio [USB Audio]

lsusb -v -d cafe:4011 | grep -A 10 "AudioStreaming"
# Should show bSubframeSize 3 and tSamFreq[ 0] 96000
```

### Quisk Configuration (`quisk_conf.py`)

This firmware is compatible with the `quisk_conf_2026.py` script.  Since the
rate is fixed at 96 kHz, ensure Quisk is configured for 96000 Hz:

```python
sample_rate = 96000
```

---

## Technical Details

### Clocking

Current configuration uses **Johnson Counter Mode**. Core 0 handles the USB
and CDC tasks, while Core 1 handles the high-priority DMA interrupts for
gapless I2S capture.

| **Core 0** | TinyUSB stack (`tud_task`); CDC tune protocol (`cdc_task`); packs DMA buffers and writes to TinyUSB audio FIFO (`audio_task`) |

### DMA ping-pong

Two DMA channels chain to each other so capture is gapless.  The ISR on
Core 1 resets the completed channel's write address and transfer count,
toggles GPIO 22, and pushes the filled buffer pointer to Core 0.

Transfer size: `DMA_SIZE_32` (32-bit words per PIO FIFO entry).

| Rate | Words / 1 ms buffer | Packed bytes (S24\_3LE) |
|------|---------------------|------------------------|
| 96 kHz | 192 (96 frames × 2 ch) | 576 |

### PIO I2S slave (`i2s_rx.pio`)

Synchronises to each stereo frame by waiting for WS HIGH then WS LOW (I2S:
WS falls at the start of the left channel).  Samples DATA on 32 consecutive
BCK rising edges per channel, shifting left into the ISR.  Autopush at 32
bits produces one FIFO word per channel.

### Si5351a local oscillator

**Johnson counter mode (v0.2 board):** CLK2 → LO_IN (4 × TuneFreq).
Set `BOARD_DIRECT_MODE false` in `main.c`.

---

## Conformance Test

```bash
python3 Research/96_kHz_CJC_5340_2026_v0.2/test_v0.2.py
```

Checks:
1. CDC device found (VID:PID `0xCAFE:0x4011`)
2. Quisk protocol: `VER`, `XTAL`, `MODE`, `RATE 96000`, `FREQ`
3. UAC1 enumeration — `WWU SDR` in `arecord -l`
4. USB descriptor — `bSamFreqType=1` on alt 1, 96 000 Hz, `bSubframeSize=3`
5. Audio capture at 96 kHz S24\_3LE
