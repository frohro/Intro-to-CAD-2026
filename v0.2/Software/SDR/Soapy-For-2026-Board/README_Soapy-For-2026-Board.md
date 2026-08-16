# Soapy2026SDR

A native [SoapySDR](https://github.com/pothosware/SoapySDR) module and hardware driver for the **WWU 2026 SDR** (RP2040 / Pico-based Software Defined Radio board).

This driver exposes the board as a standard SoapySDR receiver device, making it compatible with SDR software such as **SDR++**, **GQRX**, **GNU Radio**, and **CubicSDR**.

---

## Hardware Architecture & How It Works

```
                       ┌────────────────────────────────────────┐
                       │           WWU 2026 SDR Board           │
                       │                                        │
[ Antenna ] ──> [ Front End / Mixer ] <── [ Si5351a Clock Gen ] │
                        │                         ▲             │
                     I / Q                        │ I2C         │
                        ▼                         │             │
                [ PCM1808 ADC ]           [ RP2040 MCU ]        │
                        │ (I2S)                   │             │
                        └────────────┬────────────┘             │
                                     │ USB                      │
                       ┌─────────────┴────────────┐             │
                       │  USB Audio   │  USB CDC  │             │
                       └──────┬───────┴─────┬─────┘             │
                              │             │                   │
══════════════════════════════╪═════════════╪═══════════════════╪══════════════════
 HOST COMPUTER                │             │
                              ▼             ▼
                       [ miniaudio ]   [ SerialPort ]
                        (Direct ALSA)   (115200 baud)
                              │             │
                              └──────┬──────┘
                                     ▼
                              [ Soapy2026SDR ]
                                     │
                                     ▼
                              [ SDR++ / GQRX ]
```

### 1. Control Protocol (USB CDC Serial)
* **Discovery:** Scans available serial ports for the RP2040 USB Vendor/Product ID (`0xCAFE:0x4011` or `0xCAFE:0x4010`) and queries the firmware version via `VER`.
* **LO Tuning:** When you change frequency in your SDR software, the driver computes exact PLL MultiSynth parameters ($N, a, b, c, P_1, P_2, P_3$) using rational approximations to match the reference crystal frequency (`XTAL`).
* **Mixer Modes:**
  * **DIRECT Mode (3.8 MHz – 30 MHz):** Si5351a outputs $f_{LO} = f_{RF}$ ($N$ is even, $6 \le N \le 126$).
  * **JOHNSON Counter Mode (500 kHz – 30 MHz, v0.2 firmware):** Si5351a outputs $f_{LO} = 4 \times f_{RF}$ to drive a quadrature Johnson counter ($4 \le N \le 127$). The driver automatically selects this mode below 3.8 MHz.

### 2. Audio & I/Q Baseband Capture (USB Audio)
* **ADC:** The onboard Texas Instruments **PCM1808** captures quadrature baseband signals (Left = I, Right = Q) at 24-bit resolution (`S24_3LE`).
* **ALSA Direct Bypass:** On Linux systems running PipeWire or PulseAudio, standard audio capture can be unintentionally resampled to 48 kHz. `Soapy2026SDR` scans `/proc/asound/cards` to find the hardware card ID (`hw:CARD=...`) and binds directly using `miniaudio` with `noAutoResample = true`.
* **Buffering & Format Conversion:** Unpacks 24-bit PCM samples, normalizes them to float32 values in $[-1.0, +1.0]$, and places them into an internal lock-protected ring buffer (`RING_FRAMES = 131072`) ready for SoapySDR consumption (`SOAPY_SDR_CF32` or `SOAPY_SDR_CS16`).

---

## Dependencies & Prerequisites

### Linux (Ubuntu / Debian / Raspberry Pi OS)
```bash
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    git \
    libsoapysdr-dev \
    soapysdr-tools \
    libasound2-dev \
    libudev-dev \
    python3-soapysdr \
    python3-numpy
```

### macOS
```bash
brew install cmake soapysdr
```

---

## Building and Installing

0.  **You can just run the build script, but here is how you do it by hand if you prefer.**

1. **Clone the repository:**
   ```bash
   git clone https://github.com/frohro/Intro-to-CAD-2026
   cd Intro-to-CAD-2026/v0.2/Software/SDR/Soapy-For-2026-Board
   ```

2. **Create a build directory and compile:**
   ```bash
   mkdir build && cd build
   cmake .. -DCMAKE_BUILD_TYPE=Release
   make -j$(nproc)
   ```

3. **Install the SoapySDR module:**
   ```bash
   sudo make install
   sudo ldconfig
   ```

4. **Verify the installation:**
   Ensure the board is plugged into USB, then run:
   ```bash
   SoapySDRUtil --find="driver=2026sdr"
   ```
   You should see output similar to:
   ```text
   ######################################################
   ## Soapy SDR -- Device probe
   ######################################################
   Found WWU 2026 SDR on /dev/ttyACM0 (fw: v0.2-2026)
   ```

5. **Run the functional test:**
   From the project root directory:
   ```bash
   python3 test_driver.py --freq 7100000 --rate 48000
   ```

---

## Using with SDR++

[SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) is a cross-platform SDR receiver application that natively supports SoapySDR input sources.

### Step-by-Step Setup

1. **Launch SDR++:**
   ```bash
   sdrpp
   ```

2. **Select Source:**
   * In the top-left **Source** panel, open the source drop-down menu and select **SoapySDR**.

3. **Select the Device:**
   * In the **Device** drop-down, select **WWU 2026 SDR** (or choose `2026sdr` if manually entering arguments).
   * If it doesn't appear, click the **Refresh** button next to the device list.

4. **Configure Parameters:**
   * **Sample Rate:** Select `48000` (48 kHz) or `96000` (96 kHz).
   * **Antenna:** Keep at `RX`.
   * **Gain:** Fixed (the PCM1808 ADC uses a hardware analog front-end).

5. **Start Receiving:**
   * Click the **Play (▶)** button in the top left.
   * Tune to your desired HF frequency (e.g., `7.100 MHz` for 40m amateur radio band).
   * Choose demodulation (AM, LSB, USB, CW, etc.) and adjust the bandwidth filter in the Radio panel.

---

## Device Settings Reference

The driver exposes hardware parameters accessible through SoapySDR APIs or settings panels:

| Setting Key | Description | Values / Examples |
| :--- | :--- | :--- |
| `firmware_version` | Firmware revision reported by Pico | `v0.1`, `v0.2-2026` |
| `crystal_freq_hz` | Si5351a reference crystal frequency | `24576000.0` |
| `mixer_mode` | Active RF mixing mode | `DIRECT`, `JOHNSON` |
| `pll_status` | Return code of last frequency change | `OK,G,0` (Integer/Golden), `OK,F,0` (Fractional) |

---

## Troubleshooting

* **Permission denied on `/dev/ttyACM*`:**
  Add your Linux user to the `dialout` (or `uucp`) group:
  ```bash
  sudo usermod -a -G dialout $USER
  ```
  *(Log out and back in for changes to apply).*

* **Device not detected by `SoapySDRUtil`:**
  * Check USB connection: `lsusb` should list `16c0:05dc` or `cafe:4011`/`cafe:4010`.
  * Make sure the module was installed to the correct SoapySDR directory (usually `/usr/local/lib/SoapySDR/modules0.8/` or `/usr/lib/x86_64-linux-gnu/SoapySDR/modules0.8/`). You can inspect module search paths using `SoapySDRUtil --info`.