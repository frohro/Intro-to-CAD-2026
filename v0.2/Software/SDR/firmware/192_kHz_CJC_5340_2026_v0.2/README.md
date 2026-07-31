# WWU SDR - 192 kHz CJC5340 Firmware (v0.2)

Tested hardware: Intro-to-CAD-2026 v0.2 board (YD-RP2040 module)

This firmware targets the v0.2 board and runs fixed-rate 192 kHz capture.
The CJC5340 still provides 24-bit I2S samples, but USB streaming is converted
in firmware to dithered 16-bit stereo to fit full-speed USB bandwidth.

## What Changed Versus 96 kHz Build

- Sample rate is fixed at 192000 Hz.
- USB format changed from S24_3LE to S16_LE.
- A TPDF dither stage is applied when quantizing 24-bit samples to 16-bit.
- Audio endpoint packet size increased to 772 bytes (192 kHz async +1 frame).

## Signal Path

1. PIO captures CJC5340 I2S words (24-bit payload in 32-bit containers).
2. DMA ping-pong buffers transfer data to core 0.
3. `audio_task()` aligns signed 24-bit samples, applies TPDF dither, and
   quantizes to signed 16-bit little-endian samples.
4. TinyUSB sends the packed S16_LE stream over UAC1 isochronous IN EP.

## USB Audio Descriptor

- Streaming interface: Interface 3 alt 1
- `bSubframeSize = 2`
- `bBitResolution = 16`
- `bSamFreqType = 1`
- `tSamFreq[0] = 192000`
- `wMaxPacketSize = 772`

## Buffer Geometry

- 1 ms buffer at 192 kHz stereo = 192 frames
- DMA words per ms = 384 (L + R words)
- USB payload per ms (nominal) = 768 bytes (192 * 2 ch * 2 bytes)

## Build

Prerequisites:

```bash
export PICO_SDK_PATH=$HOME/pico-sdk
git clone https://github.com/hathach/tinyusb $HOME/tinyusb
export PICO_TINYUSB_PATH=$HOME/tinyusb
```

Compile:

```bash
cd Research/192_kHz_CJC_5340_2026_v0.2
mkdir -p build
cd build
cmake ..
make -j4
```

Flash `sdr_192k.uf2` to the Pico.

## Quisk Configuration

Use `quisk_conf_192k.py` and ensure:

```python
sample_rate = 192000
```

## Conformance Test

```bash
python3 Research/192_kHz_CJC_5340_2026_v0.2/test_v0.2.py
```

The test checks:

1. CDC protocol responses (`VER`, `XTAL`, `MODE`, `RATE`, `FREQ`).
2. ALSA enumeration as `WWU SDR`.
3. USB descriptor fields for 192 kHz S16_LE.
4. 1-second capture at 192 kHz S16_LE.
