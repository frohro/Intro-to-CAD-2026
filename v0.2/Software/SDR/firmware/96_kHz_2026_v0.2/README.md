# WWU SDR — 96 kHz UAC1 Research Firmware (v0.2)

**Tested hardware:** Intro-to-CAD-2026 v0.2 board (YD-RP2040 module)

> **Note:** This firmware targets the **v0.2 board** and is configured for **96 kHz sampling**.
> You **MUST** change the **MD1 jumper to HIGH** on the board for 96 kHz operation.
> It uses **Johnson counter mode** (clk0 -> I/Q clock) by default.
> `BOARD_DIRECT_MODE` is set to `false`.

RP2040 firmware that captures stereo I2S audio from a PCM1808 ADC and
streams it to a Linux host as a UAC1 (USB Audio Class 1) device at either
48 kHz or 96 kHz in S24\_3LE format.  A Si5351a clock synthesiser provides
the quadrature local-oscillator signal for the SDR front end.  A CDC
interface carries the Quisk tune protocol.

---

## Hardware Requirements

| Part | Description |
|------|-------------|
| YD-RP2040 | RP2040 development board (USB-C, 16 MB flash) |
| PCM1808 | 24-bit stereo ADC, I2S master mode |
| Si5351a | Clock synthesiser (I2C address 0x60, A0 pin pulled low) |
| 24.576 MHz TCXO/crystal | Reference — 512 × 48 000 = 24 576 000 Hz |

---

## GPIO Pin Assignments

All assignments are for the **Intro-to-CAD-2026 v0.1 board**.


All assignments are for the **Intro-to-CAD-2026 v0.2 board for the PCM1808 and I2C**.

| GPIO | Signal | Direction | Notes |
|------|--------|-----------|-------|
| 6 | FMT | OUT (LOW) | PCM1808 FMT — LOW = I2S standard format |
| 8 | MD1 | IN | PCM1808 MD1 — LOW = 512fs/48 kHz, HIGH = 256fs/96 kHz |
| 12 | SDA | I2C | Si5351a I2C data (100 kHz) |
| 13 | SCL | I2C | Si5351a I2C clock |
| 9 | DATA | IN (PIO) | PCM1808 DOUT — I2S serial audio |
| 10 | BCK  | IN (PIO) | PCM1808 BCK — bit clock from ADC |
| 11 | WS   | IN (PIO) | PCM1808 LRCK — word select / frame sync |
| 22 | DBG  | OUT | DMA ISR toggle — ~500 Hz square wave for logic analyser |

---

## Si5351a Clock Usage (v0.2 board)

| Clock | Signal | Frequency | Notes |
|-------|--------|-----------|-------|
| CLK0 | SCKI | 24.576 MHz | ADC Master Clock (crystal bypass) |
| CLK2 | LO_IN | 4 × TuneFreq | Input to Johnson Counter for I/Q |

BCK at 48 kHz = 3.072 MHz; at 96 kHz = 6.144 MHz.  PIO runs at 125 MHz
(32 ns/cycle) leaving ~5× margin at the fastest BCK rate.
---

## USB Audio Format — S24\_3LE

This section describes the complete data path from ADC bits to USB bytes so
that byte order, justification, and sign convention can be verified
independently.

### PCM1808 output word format

The PCM1808 drives BCK and LRCK as master outputs.  In I2S standard format
data is MSB-first, delayed one BCK from the WS edge, with 24 valid bits
followed by 8 trailing zero-pad bits per channel:

```
BCK edge:  1   2   3  ...  24  25  26 ... 32
          D23 D22 D21 ...  D0   0   0 ...  0
          ^sign/MSB        ^LSB  <-zero pad->
```

### PIO capture → 32-bit DMA word

`i2s_rx.pio` shifts DATA in on every BCK rising edge, MSB first, and
autopushes after 32 bits.  The resulting word in `buf_a[]` / `buf_b[]` is:

```
Bit:  31  30  29 ...  8   7   6 ... 0
      D23 D22 D21 ... D0   0   0 ... 0
      ^sign/MSB       ^LSB  <-zeros->
```

The sample is **left-justified** in the 32-bit word.  No firmware scaling
or bit shifting is applied at this stage.

### audio\_task() packing → 3 USB bytes

`audio_task()` packs each 32-bit DMA word to three bytes by right-shifting,
**without any gain change**:

```c
byte 0 = (w >>  8) & 0xFF   // D7..D0  — LSB
byte 1 = (w >> 16) & 0xFF   // D15..D8
byte 2 = (w >> 24) & 0xFF   // D23..D16 — MSB, sign bit in bit 7
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

The device exposes **two active alternate settings** on Interface 3 so that
the EHCI/OHCI host scheduler can allocate the appropriate isochronous
bandwidth for each rate independently:

| Alt | Rate | wMaxPacketSize | Notes |
|-----|------|---------------|-------|
| 0 | — | 0 | Zero-bandwidth (idle) |
| 1 | 48 000 Hz | 294 B | 288 B/ms + 6 B headroom (±1 sample) |
| 2 | 96 000 Hz | 582 B | 576 B/ms + 6 B headroom (±1 sample) |

```
bSubframeSize  = 3    (3 bytes per sample)
bBitResolution = 24   (24 significant bits)
bSamFreqType   = 1    (one discrete frequency per alt setting)
```

`snd-usb-audio` automatically selects alt 1 for a 48 kHz stream and alt 2
for a 96 kHz stream.  The 294 B packet size on alt 1 fits comfortably through
a Single-TT USB 2.0 hub (e.g. on a laptop EHCI controller), making 48 kHz
operation reliable on older hardware where the previous 776 B single-alt
design failed to schedule.

### TinyUSB internal buffer sizing (`tusb_config.h`)

`CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX` is set to **776 bytes** and
`CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ` to **15 360 bytes**.  The EP size
exceeds the 582 B alt-2 maximum because TinyUSB allocates a single hardware
buffer for the endpoint across all alternate settings; the SW buffer is
generous to sustain stable isochronous streaming.  The USB wire format
remains S24\_3LE (3 bytes/sample) as declared in the descriptor.

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

3. Ensure build tools are present:
   ```bash
   sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi ninja-build
   ```

### Configure and build

```bash
cd Research/96_kHz
mkdir build && cd build
cmake ..
make -j$(nproc)
```

Output: `build/sdr_96k.uf2`

### Flash

Hold BOOTSEL, connect USB, release BOOTSEL, then:
```bash
cp build/sdr_96k.uf2 /media/$USER/RPI-RP2/
```

---

## Running with Quisk

1. Point Quisk at `quisk_conf_96k.py` (**Config → Radio → Hardware file**).

2. The active sample rate is set at the top of the config file:
   ```python
   sample_rate = 96000   # or 48000
   ```
   No other changes are needed to switch rates.

3. The capture device is set to `"alsa:WWU SDR USB Audio (hw:3,0)"` — the
   PortAudio ALSA device string as Quisk sees it.  The `hw:X,0` card number
   varies by machine.  If capture does not start, open **Config → Radio →
   Sound**, select the `WWU SDR` entry from the drop-down, and Quisk will
   update the config file automatically.  Verify the device is enumerated:
   ```bash
   arecord -l | grep SDR
   ```

4. `lin_latency_millisecs = 500` provides PipeWire headroom for transient CPU
   spikes at 96 kHz (which doubles DSP and FFT load vs. 48 kHz).

---

## CDC Serial Protocol

The firmware presents `/dev/ttyACM0` (or `/dev/ttyACM1`) carrying a
line-oriented ASCII protocol.  All lines terminate with `\r\n`.

| Command | Response | Notes |
|---------|----------|-------|
| `VER` | `VER,SDR C firmware 2.0\r\nOK\r\n` | Firmware version string |
| `XTAL` | `XTAL,24576000\r\nOK\r\n` | Crystal frequency in Hz |
| `MODE` | `MODE,DIRECT\r\nOK\r\n` | Board topology (DIRECT on v0.1) |
| `RATE,<hz>` | `OK\r\n` | Informational; rate is now determined by the USB alt setting snd-usb-audio selects (48000 → alt 1, 96000 → alt 2) |
| `FREQ,` | `<hz>\r\nOK,<type>,0\r\n` | Query current LO frequency |
| `FREQ,<hz>,<N>,<a>,<b>,<c>,<P1>,<P2>,<P3>` | `<hz>\r\nOK,G,0\r\n` | Set LO; `G`=integer PLL, `F`=fractional |

Startup sentinel (sent when DTR asserted; re-sent on Ctrl-D):
```
SDR ready - waiting for precise integer commands...\r\n
```

---

## Architecture

### Dual-core split

| Core | Responsibilities |
|------|-----------------|
| **Core 1** | Owns `DMA_IRQ_0`; runs DMA ping-pong ISR; posts filled buffer pointers to Core 0 via inter-core FIFO |
| **Core 0** | TinyUSB stack (`tud_task`); CDC tune protocol (`cdc_task`); packs DMA buffers and writes to TinyUSB audio FIFO (`audio_task`) |

### DMA ping-pong

Two DMA channels chain to each other so capture is gapless.  The ISR on
Core 1 resets the completed channel's write address and transfer count,
toggles GPIO 22, and pushes the filled buffer pointer to Core 0.

Transfer size: `DMA_SIZE_32` (32-bit words per PIO FIFO entry).

| Rate | Words / 1 ms buffer | Packed bytes (S24\_3LE) |
|------|---------------------|------------------------|
| 48 kHz | 96 (48 frames × 2 ch) | 288 |
| 96 kHz | 192 (96 frames × 2 ch) | 576 |

Both buffers are always allocated at the 96 kHz maximum (192 words).

### Rate-change handshake

1. Core 0 updates `g_words_per_buf`, toggles GPIO 11 (PCM1808 MD1), then
   pushes sentinel `0xFFFFFFFF` into the inter-core FIFO.
2. Core 1 detects the sentinel, disables `DMA_IRQ_0`, aborts both channels,
   reprograms `trans_count`, re-enables IRQ, restarts channel A, then pushes
   ack `0xFFFFFFFE`.
3. Core 0 spins until the ack arrives (100 ms timeout per poll).

### PIO I2S slave (`i2s_rx.pio`)

Synchronises to each stereo frame by waiting for WS HIGH then WS LOW (I2S:
WS falls at the start of the left channel).  Samples DATA on 32 consecutive
BCK rising edges per channel, shifting left into the ISR.  Autopush at 32
bits produces one FIFO word per channel.

### Si5351a local oscillator

**DIRECT topology (v0.1 board):** CLK0 → I channel, CLK1 → Q channel.
An exact 90° phase offset is set on CLK1 via `CLK1_PHOFF = N` (the output
divider).  Quisk computes optimal PLL parameters on the host and sends
pre-computed `N, P1, P2, P3` via `FREQ,...` to avoid floating-point on the
Pico.

**Johnson counter mode (v0.2 board):** Not yet supported in this firmware.
Set `BOARD_DIRECT_MODE false` in `main.c` and update `si5351_set_freq_regs()`
to omit the CLK1 phase-offset write.

---

## Conformance Test

```bash
python3 Research/96_kHz/test_96k.py
```

Checks:
1. CDC device found (VID:PID `0xCAFE:0x4011`)
2. Quisk protocol: `VER`, `XTAL`, `MODE`, `RATE 96000`, `FREQ`
3. UAC1 enumeration — `WWU SDR` in `arecord -l`
4. USB descriptor — `bSamFreqType=1` on both alt settings, 48 000 Hz on alt 1,
   96 000 Hz on alt 2, `bSubframeSize=3`
5. Audio capture at 96 kHz S24\_3LE
6. Audio capture at 48 kHz S24\_3LE
