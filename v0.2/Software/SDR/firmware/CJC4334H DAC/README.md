# CJC4334H DAC & PAM8908 Headphone Amplifier Firmware Test

Firmware test suite written with the Raspberry Pi Pico SDK for testing the **CJC4334H I2S Audio DAC** and **PAM8908 Headphone Amplifier** circuit on the Intro-to-CAD-2026 board (YD-RP2040).

---

## Hardware Configuration & Pinout

| Signal | RP2040 Pin | Hardware Connection / Notes |
| :--- | :--- | :--- |
| **I2S SCK (BCLK)** | `GP16` | Bit Clock input to CJC4334H (JP34 bridged) |
| **I2S WS (LRCK)** | `GP17` | Word Select / Frame Clock to CJC4334H (JP33 bridged) |
| **I2S SD (DOUT)** | `GP18` | Serial Data input to CJC4334H (JP32 bridged) |
| **MCLK** | `24.576 MHz` | **Default**: Onboard 24.576 MHz crystal oscillator via JP6 (Pins 1-2 bridged) |
| **Optional MCLK** | `GP15` | Software PWM MCLK generator if jumper swapped to `GP15` (JP6 Pins 2-3) |
| **Audio Output** | `J9 Jack` | 3.5mm Headphone Jack driven by PAM8908 stereo amplifier |

---

## Technical Features

1. **Strict 64fs I2S Timing (`i2s_tx.pio`)**:
   - Zero-overhead 128-cycle PIO state machine (64 cycles Left, 64 cycles Right).
   - 100% symmetric 50.0% duty cycle SCLK ($t_{\text{high}} = t_{\text{low}}$).
   - 1-bit I2S frame delay per standard I2S protocol.

2. **Continuous Zero-Gap Hardware DMA Ping-Pong Streaming**:
   - Dual-channel chained DMA buffers (`dma_chan_a` and `dma_chan_b`).
   - Hardware IRQ refilling background buffer during active playback (**0 nanoseconds gap**).
   - Continuous phase Direct Digital Synthesis (DDS) preventing pops, clicks, or 0.5ms buffer dropouts.

3. **Cubic Perceptual Volume Control**:
   - Master volume scaled via $(\text{Volume \%})^3$ to provide fine, comfortable listening levels on the capless PAM8908 headphone amplifier.
   - Default volume set to a clean, comfortable **5%**.

---

## Building the Firmware

Ensure the Raspberry Pi Pico SDK is installed and `PICO_SDK_PATH` is set in your environment.

```bash
cd "v0.2/Software/SDR/firmware/CJC4334H DAC"
mkdir -p build
cd build
cmake ..
make -j8
```

This generates `cjc4334h_dac_test.uf2` in the `build/` directory.

---

## Flashing to YD-RP2040

1. Hold down the **BOOTSEL** button on the YD-RP2040 board while connecting it via USB.
2. Copy `cjc4334h_dac_test.uf2` to the `RPI-RP2` drive.
3. The board will automatically reboot and execute the firmware (onboard LED will blink a 500ms heartbeat).

---

## Interactive USB Terminal Controls

Open a serial terminal connected to the YD-RP2040 USB port (e.g. `minicom -D /dev/ttyACM0` or VSCode Serial Monitor at 115200 baud).

### Interactive Menu Commands:
- `1` : Select **1 kHz Stereo Sine Wave** (Default test tone).
- `2` : Select **Stereo L/R Channel Separation Test** (440 Hz Left / 880 Hz Right alternating every second).
- `3` : Select **Logarithmic Frequency Sweep** (20 Hz to 20 kHz continuous sweep).
- `4` : Select **Polyphonic A-Major Chord** (440 Hz + 554.37 Hz + 659.25 Hz triad).
- `5` : **Mute / Silence** output.
- `+` / `-` : Fine volume adjustment (**+1% / -1%**).
- `*` / `/` : Coarse volume adjustment (**+5% / -5%**).
- `f` / `F` : Increase / Decrease base sine wave frequency (**+50 Hz / -50 Hz**).
- `m` : Toggle GP15 MCLK PWM output ON / OFF.
- `h` : Redisplay interactive menu.
