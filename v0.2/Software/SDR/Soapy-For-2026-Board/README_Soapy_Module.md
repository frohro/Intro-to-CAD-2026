
# WWU 2026 SDR - SoapySDR Module

This repository contains the SoapySDR driver for the **WWU 2026 SDR Board** (v0.1 and v0.2). 

The WWU 2026 SDR is a custom hardware design utilizing an RP2040 (Raspberry Pi Pico) for control, a Si5351a for the Local Oscillator (LO), and a PCM1808 stereo 24-bit ADC for I/Q audio capture.

This driver bridges the USB CDC (Serial) control protocol and the UAC1 (USB Audio Class 1) data stream into a unified `SoapySDR::Device`, allowing the hardware to be used with standard SDR applications like SDR++, GNU Radio, and custom C++/Python scripts.

## Features

* **Si5351a Phase Noise Optimization:** The driver automatically searches for Integer (Golden) PLL multipliers to ensure the cleanest possible LO signal with minimal spurious emissions (see below).
* **Direct ALSA Capture:** Uses `miniaudio` to bypass the Linux PipeWire/PulseAudio servers, ensuring direct access to the `hw:` ALSA device. This prevents unauthorized OS-level resampling and guarantees a bit-perfect 24-bit stream.
* **Dual Mixer Modes:** Supports both `DIRECT` (1x LO) and `JOHNSON` (4x LO) mixer modes, with auto-switching based on frequency (v0.2 firmware).
* **Multi-Platform:** Built-in serial port enumeration supports Linux (`ttyACM*`), macOS (`cu.usbmodem*`), and Windows (`COM*`).
* **High-Speed Ring Buffer:** Internal thread-safe float32 ring buffer prevents sample drops during heavy CPU loads.
* **Optional Manual Si5351 Tuning:** Applications can pass a complete PLL/register solution through `setFrequency()` keyword arguments. This enables a higher-level frequency plan to select known-good integer or low-spur solutions while preserving automatic tuning as the fallback.

## Si5351a Phase Noise Optimization (Integer vs. Fractional)

The Si5351a clock generator uses a PLL multiplier ($M = a + b/c$) to generate the VCO frequency. Using a fractional multiplier ($b > 0$) introduces phase noise and spurious emissions (spurs) into the radio spectrum. Using a pure integer multiplier ($b = 0$) results in a vastly superior, cleaner signal.

When you tune the SDR, the C++ driver host-side performs a mathematical search:
1. **The Integer Search:** It sweeps through all valid output dividers ($N$) to see if the requested frequency can be achieved using a pure integer multiplier ($M = a$). 
2. **The Fractional Fallback:** If an integer match is impossible for the exact requested frequency, the driver calculates the closest rational approximation using a fractional multiplier to guarantee tuning accuracy.

The driver sends the calculated parameters to the RP2040, which configures the Si5351a and responds with a status code indicating which mode was achieved:
* `OK,G,<offset>`: **Golden (Integer)** mode achieved. The LO is operating with minimum phase noise.
* `OK,F,<offset>`: **Fractional** mode used. 

The `offset` indicates any microscopic rounding error (in Hz) between the requested frequency and the actual hardware output.

## Manual Si5351 parameters from applications

The normal path remains automatic: `_computeSi5351()` selects a valid solution for the requested RF frequency. For hardware characterization or a coordinated multi-mode frequency plan, callers may pass all of these keyword arguments to `setFrequency()`:

| Key | Meaning |
|---|---|
| `si5351_hz` | Si5351 output frequency sent to firmware, in Hz |
| `N` | Output divider |
| `a` | Integer PLL multiplier component |
| `b` | Fractional PLL numerator |
| `c` | Fractional PLL denominator |
| `p1`, `p2`, `p3` | Calculated Si5351 register values |

The override is all-or-nothing. If any field is missing, the driver ignores the partial override and uses `_computeSi5351()`. The driver must validate ranges before sending the command: `N`, `a`, `b`, `c`, and the register values must be valid for the active DIRECT or JOHNSON mode, and the resulting VCO must remain within the Si5351 operating range. Invalid values should produce a clear error rather than programming the board.

The Soapy driver implementation requires the following interface change:

```cpp
void _programSi5351(double rfHz, const SoapySDR::Kwargs &args);
```

`setFrequency()` passes its `args` parameter to `_programSi5351()`. `_programSi5351()` checks for the complete key set, converts values with checked integer parsing, and either constructs the `FREQ,...` command from the supplied values or calls `_computeSi5351(rfHz)`.

The harvester's JSON-to-Soapy mapping should be:

```cpp
SoapySDR::Kwargs tuneArgs;
for (const auto &[key, value] : sdr.si5351.items())
    tuneArgs[key] = value.dump();
device->setFrequency(SOAPY_SDR_RX, 0, sdr.centerFreq, tuneArgs);
```

This keeps the JSON frequency plan independent of the driver while allowing the driver to remain the final authority on safety validation. A future frequency-planning tool can populate this block by searching for integer-multiplier (`b = 0`) solutions, checking mixer-mode constraints, and selecting the solution that leaves every requested mode inside the 96 kHz capture window.

## Prerequisites

To compile this driver, you will need the SoapySDR development libraries and a standard C++ build chain.

**On Ubuntu / Debian:**
```bash
sudo apt update
sudo apt install cmake g++ libsoapysdr-dev python3-soapysdr
```

## Building and Installing

1. Clone the repository and navigate to the module directory.
2. Create a build directory and compile using CMake:
```bash
mkdir build
cd build
cmake ..
make -j4
sudo make install
```
3. Update the system library cache (Linux only):
```bash
sudo ldconfig
```

## Verifying the Installation

Plug in your WWU 2026 SDR board. Run the SoapySDR utility command to probe the USB bus:

```bash
SoapySDRUtil --find="driver=2026sdr"
```
You should see output similar to this:
```
Found device 0
  driver = 2026sdr
  label = WWU 2026 SDR (/dev/ttyACM0)
  serial_port = /dev/ttyACM0
  version = 0.2
```

## Running the Test Script

A Python test script is included to verify both the serial LO tuning and the audio data stream. From the root directory, run:

```bash
python3 test_driver.py --freq 7100000 --rate 48000
```
This script will configure the board to 7.1 MHz, capture 0.2 seconds of I/Q data, and calculate the mean power (dBFS) and DC offset. It will also print the `PLL status` so you can verify if the Si5351a achieved a Golden (Integer) lock. A `PASS ✓` at the end indicates the driver is fully functional.

## Advanced Usage (Device Arguments)

When instantiating the device in code (or via applications like SDR++), you can pass optional key-value arguments (`Kwargs`) to override default behaviors. This is particularly useful when using **multiple boards simultaneously**.

| Argument | Description | Default |
| :--- | :--- | :--- |
| `serial_port` | Forces the driver to use a specific serial port (e.g., `/dev/ttyACM1`), skipping auto-discovery. | Auto-discovered |
| `audio_label` | The ALSA card label used to select the correct audio card in `/proc/asound/cards`. Crucial when running multiple boards to ensure the audio stream matches the serial port. When supplied explicitly, a missing label is an error; the driver will not silently fall back to another board. | `"WWU SDR"` |

**Example of explicitly mapping a specific board:**
```cpp
SoapySDR::Kwargs args;
args["driver"] = "2026sdr";
args["serial_port"] = "/dev/ttyACM1";
args["audio_label"] = "S2026_1"; // Maps to ALSA hw:CARD=S2026_1,DEV=0
SoapySDR::Device *sdr = SoapySDR::Device::make(args);
```

## Under the Hood: Firmware Protocol

The driver communicates with the RP2040 firmware via a simple human-readable CDC serial protocol (115200 baud, 8N1):
* `VER` -> returns `VER,<version>`
* `XTAL` -> returns `XTAL,<hz>`
* `RATE,<48000|96000>` -> returns `OK`
* `MODE,<DIRECT|JOHNSON>` -> returns `OK`
* `FREQ,<si5351_hz>,<N>,<a>,<b>,<c>,<P1>,<P2>,<P3>` -> returns `OK,[G|F],<offset>`

The complex Si5351a register math (rational approximation) is handled entirely by the C++ driver host-side, keeping the microcontroller firmware lightweight and highly responsive.
```
