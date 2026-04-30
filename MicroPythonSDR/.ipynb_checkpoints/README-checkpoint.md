# MicroPythonSDR — Intro-to-CAD-2026 / Cadyn Pico SDR

MicroPython firmware that turns a Raspberry Pi Pico (RP2040) into the
local-oscillator (LO) controller for a Quadrature Sampling Detector (QSD)
SDR receiver.  The Pico tunes the Si5351/MS5351M clock generator to produce
two clocks 90° apart (CLK0 = I, CLK1 = Q) that drive the QSD mixer.  Analog
I/Q audio from the mixer goes to the left/right channels of an external
soundcard, and Quisk (or any compatible SDR program) demodulates it on the PC.

```
RF Antenna
    │
    ▼
H1102NLT (input balun)
    │
    ▼
QSD mixer ◄── CLK0 (0°)  ┐
          ◄── CLK1 (90°) ┘  MS5351M / Si5351A  ◄── I²C (SDA/SCL) ── Pico
    │
    ▼
OPA1612 (I/Q audio amp)
    │
    ▼
3.5 mm stereo jack  ──────► PC soundcard L (I) / R (Q)
                                │
                                ▼
                           Quisk SDR software
                                │
                         USB serial ──► Pico  (FREQ,<hz> commands)
```

---

## Hardware

### Intro-to-CAD-2026 board

| Item | Detail |
|---|---|
| MCU | Raspberry Pi Pico (RP2040) |
| Clock generator | MS5351M (Si5351A-compatible, LCSC C1509083), I²C addr `0x60` |
| I²C pins | **SDA = GPIO 12**, **SCL = GPIO 13** (I²C bus 0) |
| Reference oscillator | 24.576 MHz internal to MS5351M |
| Mixer / amp | OPA1612 dual op-amp in QSD configuration |
| RF input balun | H1102NLT Ethernet transformer |
| Audio output | 3.5 mm stereo jack → external soundcard |
| PCM1808 ADC | Present on board; *not used by this firmware* (future Phase 2) |
| 24.576 MHz CMOS osc | YC24.576MUBCE2O — feeds PCM1808 SCKI only |
| OLED display | SSD1306 on same I²C bus; *not used by this firmware* |

### Cadyn's Pico SDR (reference build)

| Item | Detail |
|---|---|
| Clock generator | Si5351A-B-GT, I²C addr `0x60` |
| I²C pins | **SDA = GPIO 4**, **SCL = GPIO 5** (I²C bus 0) |
| Reference oscillator | ~25 MHz internal crystal (calibrated value in `config.py`) |

---

## File layout

```
MicroPythonSDR/
├── main.py          Entry point — reads config, initialises Si5351, starts radio loop
├── config.py        Board selector (edit BOARD here before deploying)
├── si5351.py        Si5351/MS5351M MicroPython driver
├── radio.py         Radio class — frequency management + Quisk serial protocol
├── Cadyn-Pico-SDR/  Symlink to Cadyn's original code (read-only reference)
└── README.md        This file
```

---

## Deployment

### 1. Install MicroPython on the Pico

Flash the latest MicroPython UF2 from <https://micropython.org/download/RPI_PICO/>.

### 2. Select your board

Edit `config.py` and set:

```python
BOARD = "INTRO_CAD_2026"   # or "CADYN_PICO_SDR"
```

### 3. Copy files to the Pico

Using **MicroPico** (VS Code extension) or **Thonny**:

Copy these four files to the root (`/`) of the Pico filesystem:

```
main.py
config.py
si5351.py
radio.py
```

The Pico runs `main.py` automatically on power-up.

### 4. Verify I²C connectivity

Before running the full firmware, run an I²C scan to confirm the MS5351M
responds at address `0x60`:

```python
from machine import I2C, Pin
i2c = I2C(0, freq=400_000, scl=Pin(13), sda=Pin(12))
print([hex(a) for a in i2c.scan()])   # should include 0x60
```

Paste this into the Pico REPL via Thonny or `mpremote`.

---

## Quisk setup (PC side)

1. Install Quisk: `pip install quisk` (or from <https://james.ahlstrom.name/quisk/>).

2. Run Quisk pointing at `quisk_conf_intro_cad.py` from this directory:
   ```
   quisk --conf /path/to/MicroPythonSDR/quisk_conf_intro_cad.py
   ```
   The file configures:
   - Serial port `/dev/ttyACM0` (or `ttyACM1`) at 115 200 baud
   - PulseAudio soundcard for I/Q capture at 48 kHz
   - `open()` queries `XTAL` to get the crystal reference frequency, sends `RATE,48000`
   - `ChangeFrequency()` searches for the best golden LO on the PC, sends `FREQ,<lo>` to the Pico

   If you change the soundcard sample rate, update `sample_rate` in
   `quisk_conf_intro_cad.py`.

3. Start Quisk.  It will open the serial port, confirm firmware version,
   read the crystal frequency, send the sample rate, and begin sending
   `FREQ,<lo>` commands as you tune.  The Pico responds with the frequency
   and an `OK,G/F/X,<value>` status line.

### Serial command reference

| Command | Response | Notes |
|---|---|---|
| `FREQ,<hz>\n` | `<hz>\nOK,G,<signed_offset>\n` | Tune; `G` = integer PLL, signed_offset = f_lo − f_requested (Hz) |
| `FREQ,<hz>\n` | `<hz>\nOK,F,0\n` | Tune; `F` = fractional PLL (exact frequency, score omitted) |
| `FREQ,<hz>\n` | `<hz>\nOK,X,0\n` | Tune; `X` = fallback, VCO out of spec |
| `FREQ,\n` | `<current hz>\nOK,...\n` | Query current frequency |
| `RATE,<hz>\n` | `OK\n` | Set soundcard sample rate (used for NeoPixel threshold) |
| `XTAL\n` | `<crystal_hz>\nOK\n` | Query reference crystal frequency (Hz, float) |
| `VER\n` | `SDR Pi Pico version 0.1\nOK\n` | Firmware identification |

**Signed offset** (`OK,G,<offset>`): positive means the LO is *above* the
requested frequency → the DC spike appears *below* the station in the audio
passband.  Negative means the LO is *below* → spike is *above* the station.
Quisk PC code uses this to track exactly where the DC spike lands.

---

## Frequency range

| Band segment | Output divider (N) | VCO range |
|---|---|---|
| 3.8 – 7.999 MHz | 100 | 380 – 800 MHz |
| 8.0 – 10.999 MHz | 80 | 640 – 880 MHz |
| 11.0 – 14.999 MHz | 50 | 550 – 750 MHz |
| 15.0 – 30.0 MHz | 30 | 450 – 900 MHz |

### Frequency selection algorithm

The firmware uses a two-priority ranked search over all valid output dividers N
to find the best LO setting for each requested frequency.

**Score metric**

For each candidate N, the PLL feedback multiplier is:

```
M_exact = freq × N / F_ref
```

Define `frac(M) = M_exact − floor(M_exact)`, then:

```
score = min(frac(M), 1 − frac(M))   # 0 = integer (best), 0.5 = worst
```

Score 0 means M is a pure integer → zero fractional spurs.  Score 0.5 means M
sits exactly halfway between integers → worst fractional spur energy.

**Search bounds**

- **N_min = ceil(600 MHz / freq)**, **N_max = min(127, floor(900 MHz / freq))**
  — keeps VCO in the Si5351 specified range of 600–900 MHz
- **N ≤ 127** — the PHOFF register is 7-bit; the 90° formula PHOFF = N only
  works when N fits in 7 bits

**LO selection is now on the Quisk PC side**

`quisk_conf_intro_cad.py::ChangeFrequency()` queries the crystal frequency
from the Pico on startup (`XTAL` command) and runs the full golden-search in
Python each time `tune` changes.  It then sends the exact LO frequency to the
Pico via `FREQ,<lo>`.  The Pico just programs what it is told.

**Sweet-spot placement (normal tune)**

Among all golden LOs within ±half_bw of `tune`, the Quisk code picks the one
where:
```
cost = | |lo_offset| − sample_rate/4 |
```
is minimised.  This places the DC spike near the passband *edge* (at ±12 kHz
for a 48 kHz card), away from both DC and the station of interest.

**Arrow-press hopping**

When an arrow button is pressed, the code detects the direction from
`sign(vfo_in − last_lo)` and hops to the nearest golden LO in that direction.
If none exists, a fractional ±quarter-bandwidth shift is used so the DC spike
always moves (it never stays on the station).

**Fallback (fractional PLL)**

When no golden LO exists within ±half_bw of `tune`, the Pico uses its
fractional PLL to hit the requested frequency exactly.  Score 0 = best (M is
very close to an integer), 0.5 = worst.  NeoPixel shows yellow (score < 0.10)
or red (score ≥ 0.10).

Example: 14.074 MHz (20 m FT8) — no golden within ±24 kHz → fractional,
N=49, M=28.061, score=0.061 (yellow).

This typically gives **10–50× better spur scores** vs. the original fixed four-N
scheme and provides a dense set of golden frequencies across the HF range.

### NeoPixel status LED

The YD-RP2040 V1.3 carries a WS2812B RGB NeoPixel on **GPIO 23**.  The
firmware updates its colour every time the LO frequency is set:

| Colour | Meaning |
|---|---|
| **Green** | Integer PLL — golden frequency, zero fractional spurs |
| **Yellow** | Fractional PLL, low spur score (score < 0.10) |
| **Red** | Fractional PLL, high spur score (score ≥ 0.10) |
| **Dim purple** | Fallback / VCO out of spec / initialising |

The Quisk status bar (bottom of the screen) also shows the same information
as text, updated via the `HeartBeat()` callback in `quisk_conf_intro_cad.py`.
Example messages:
- `GOLDEN  +3888 Hz  (integer PLL — no spurs)` — LO is 3888 Hz above tune
- `GOLDEN  -970 Hz  (integer PLL — no spurs)` — LO is 970 Hz below tune
- `frac  (exact freq)` — fractional PLL, LO = exact requested frequency
- `fallback  (VCO out of spec)` — below ~3.8 MHz or hardware limit hit

### Using the Quisk IF arrows for golden-frequency hopping

The two blue arrow buttons in Quisk (labelled **←** and **→** near the
passband display) now hop the LO to the *next* golden (integer-PLL) frequency
in the pressed direction, keeping the station you are listening to fixed
inside the audio passband:

- **→ (right arrow)**: hops to the next golden LO *above* the current one.
- **← (left arrow)**: hops to the next golden LO *below* the current one.
- If no golden exists in that direction within the passband, the LO shifts by
  `sample_rate / 4` (12 kHz for 48 kHz card) to push the DC spike away from
  centre.  NeoPixel shows yellow or red.
- The station's RF frequency (`tune`) never changes — only the LO moves.
  Quisk adjusts the audio IF offset automatically.

This lets you A/B compare integer-PLL (green, no spurs) against fractional
(yellow/red) while keeping the station centred in the passband.

### Key ham frequencies — PLL mode (F_ref = 24.576 MHz, half\_bw = 24 kHz)

Computed from the golden-search algorithm in `quisk_conf_intro_cad.py`.
INTEGER = pure integer PLL (NeoPixel green); fractional = best fractional PLL
(yellow or red).  **Signed offset** = f_lo − f_requested; positive means the
LO is above tune, raising the DC spike below the station in the audio passband.

| Frequency | Band / Mode | PLL | M | N | Signed LO offset | Score |
|---|---|---|---|---|---|---|
| 5.000 000 MHz | WWV 5 MHz | INTEGER | 25 | 123 | +4 878 Hz | — |
| 6.000 000 MHz | SW 6 MHz | INTEGER | 31 | 127 | +1 134 Hz | — |
| 7.074 000 MHz | 40 m FT8 | INTEGER | 36 | 125 | +3 888 Hz | — |
| 7.200 000 MHz | 40 m SSB | INTEGER | 29 | 99 | +970 Hz | — |
| 7.290 000 MHz | 40 m AM | INTEGER | 35 | 118 | +509 Hz | — |
| 9.500 000 MHz | SW 9.5 MHz | INTEGER | 29 | 75 | +2 720 Hz | — |
| 10.000 000 MHz | WWV 10 MHz | INTEGER | 35 | 86 | +1 861 Hz | — |
| 10.100 000 MHz | 30 m CW | INTEGER | 30 | 73 | +274 Hz | — |
| 10.140 000 MHz | 30 m FT8 | INTEGER | 33 | 80 | +2 400 Hz | — |
| 14.074 000 MHz | 20 m FT8 | fractional | 28.061 | 49 | 0 (exact) | 0.0610 |
| 14.225 000 MHz | 20 m SSB | INTEGER | 33 | 57 | +3 211 Hz | — |
| 15.000 000 MHz | WWV 15 MHz | INTEGER | 36 | 59 | +4 475 Hz | — |
| 18.100 000 MHz | 17 m FT8 | INTEGER | 28 | 38 | +8 632 Hz | — |
| 18.130 000 MHz | 17 m SSB | INTEGER | 31 | 42 | +9 429 Hz | — |
| 21.074 000 MHz | 15 m FT8 | INTEGER | 30 | 35 | +8 857 Hz | — |
| 21.300 000 MHz | 15 m SSB | INTEGER | 26 | 30 | +800 Hz | — |
| 24.915 000 MHz | 12 m FT8 | fractional | 25.345 | 25 | 0 (exact) | 0.3449 |
| 24.940 000 MHz | 12 m SSB | fractional | 25.370 | 25 | 0 (exact) | 0.3703 |
| 28.074 000 MHz | 10 m FT8 | INTEGER | 32 | 28 | +12 857 Hz | — |
| 28.400 000 MHz | 10 m SSB | fractional | 30.046 | 26 | 0 (exact) | 0.0456 |

**Note on 12 m (≈ 24.9 MHz):** With F_ref = 24.576 MHz, the available N range
for this band is very narrow (N = 25–36) and none produce an M close to an
integer.  The NeoPixel will show **red** on 12 m.

**Note on signed LO offsets:** The Quisk golden-search algorithm chooses the
golden LO closest to a \(|offset| = sample\_rate/4 = 12\,000\) Hz sweet spot.
The table above shows the best initial pick; arrow presses will hop to other
golden LOs in the same list.

---

## How quadrature generation works

The Si5351/MS5351M generates Fout using two stages:

```
Fout = F_ref × M / N
```

where  
**M** = PLL feedback multiplier (fractional: `a + b/c` encoded in p1/p2/p3)  
**N** = output Multisynth divider (**must be integer** for quadrature)

### Why N must be an integer

The PHOFF register (Reg 165/166) sets the phase delay in units of
¼ VCO period.  Setting `CLK1_PHOFF = N` produces:

```
delay = N × (1/4 VCO period) = 1/4 output period = 90°
```

This identity holds **only** when N is a whole number.
`radio.py` always passes an integer `mult` to `set_frequency()`, and
`si5351.py` uses `clock.configure_integer(pll, mult)` for the output stage,
so the quadrature condition is always satisfied.

### Why the PLL uses fractional M

With `F_ref = 24.576 MHz` and integer-only M, the minimum tuning step
would be `F_ref / N`.  With N = 100 that is **245.76 kHz per step** —
far larger than the ~48 kHz audio bandwidth of a soundcard SDR.
Fractional M lets the VCO be placed at any frequency with sub-Hz
resolution, which is what Quisk's `FREQ,<hz>` command requires.

---

## Crystal calibration

The MS5351M's internal oscillator frequency is not exactly 24.576 MHz.
To calibrate:

1. Tune to a known-accurate signal (WWV at 5, 10, or 15 MHz is convenient;
   a calibrated signal generator works too).

2. In Quisk, note the offset between the displayed frequency and the zero-beat
   or carrier tone.  At 10 MHz a 100 Hz error means the crystal is off by
   `100 / 10e6 = 10 ppm`.

3. Compute the corrected crystal frequency:
   ```
   crystal_corrected = 24_576_000 × (1 + error_Hz / nominal_Hz)
   ```

4. Update `CRYSTAL_FREQ` in `config.py` with the new value.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| I²C scan returns empty list | Wiring or pull-up resistors | Check SDA/SCL connections; the RP2040 has internal weak pull-ups but strong external pull-ups (4.7 kΩ) are recommended |
| `assert` error on startup | VCO out of range for requested frequency | Verify frequency is within 3.8 – 30 MHz |
| Quisk sees no serial port | Wrong `/dev/ttyACM*` | Check `quisk_conf_sdr_pi_pico.py`; try `ls /dev/ttyACM*` |
| Image rejection poor | Wrong phase offset | Confirm CLK0 and CLK1 are 90° apart with an oscilloscope |
| Frequency off by fixed amount | Crystal not calibrated | Follow crystal calibration procedure above |
| Startup hang (rare) | SYS_INIT poll | Power-cycle the board; if persistent, check 3.3 V rail |

---

## Future work (Phase 2)

The Intro-to-CAD-2026 board includes a **PCM1808 stereo I²S ADC** clocked
by the 24.576 MHz CMOS oscillator (48/96 kHz sample rates).  Once the LO
firmware is proven, a Phase 2 extension could:

- Read I²S data from the PCM1808 on the Pico's PIO state machines
- Stream audio over USB as a USB Audio Class device
- Eliminate the external soundcard entirely

---

## License

Driver code derived from the Adafruit CircuitPython Si5351 library
(Tony DiCola, MIT License).  All modifications and additions are also MIT.
