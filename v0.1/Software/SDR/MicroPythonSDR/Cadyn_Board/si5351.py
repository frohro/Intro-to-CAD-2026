# The MIT License (MIT)
#
# Copyright (c) 2017 Tony DiCola for Adafruit Industries
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
"""
Si5351 / MS5351M MicroPython driver (ported from Adafruit CircuitPython library).

Modifications vs. Cadyn-Pico-SDR/si5351.py:
  - crystal_freq is now a constructor parameter instead of a module constant,
    so the same driver works on any board regardless of the reference oscillator.
  - Both _PLL frequency calculations use self._si5351._crystal_freq, so
    clock.frequency reports accurately after set_frequency() is called.
  - Fixed truncated outputs_enabled setter (else branch that was missing).

Integer vs. fractional dividers
---------------------------------
The Si5351/MS5351M architecture has two stages:

  Fout = F_ref × M / N

where
  M  = PLL feedback multiplier (stored in MSNA/MSNB registers, p1/p2/p3)
  N  = output Multisynth divider (stored in MS0..MS5 registers)

Quadrature requirement:
  N MUST be an integer.  The PHOFF register (Reg 165/166) specifies the
  phase delay in units of 1/4 VCO period.  Setting CLK1_PHOFF = N gives
  a delay of N × (1/4 VCO period) = 1/4 output period = exactly 90°.
  This identity holds only when N is a whole number — and it IS, because
  set_frequency() always calls clock.configure_integer(pll, mult).

Why M uses fractional mode:
  With F_ref = 24.576 MHz and integer-only M, the minimum tuning step is
  F_ref / N.  With N = 100, that is 245.76 kHz — far larger than the ~48 kHz
  audio bandwidth of a typical soundcard SDR.  Fractional M (p2/p3 ≠ 0)
  lets the VCO be placed at any frequency with sub-Hz resolution, which is
  what Quisk's "FREQ,<hz>" command expects.  The small phase-noise penalty
  of the fractional PLL is insignificant for HF SSB/CW/FT8 work.
"""
import math
import time

from micropython import const

import machine
from machine import I2C
from machine import Pin


# Internal constants
_SI5351_ADDRESS                             = const(0x60)
_SI5351_READBIT                             = const(0x01)
# Default fallback crystal frequency — override with the crystal_freq
# constructor argument for your specific board.
_SI5351_CRYSTAL_FREQUENCY_DEFAULT           = 25_000_000.0
_SI5351_REGISTER_0_DEVICE_STATUS            = const(0)
_SI5351_REGISTER_1_INTERRUPT_STATUS_STICKY  = const(1)
_SI5351_REGISTER_2_INTERRUPT_STATUS_MASK    = const(2)
_SI5351_REGISTER_3_OUTPUT_ENABLE_CONTROL    = const(3)
_SI5351_REGISTER_9_OEB_PIN_ENABLE_CONTROL   = const(9)
_SI5351_REGISTER_15_PLL_INPUT_SOURCE        = const(15)
_SI5351_REGISTER_16_CLK0_CONTROL            = const(16)
_SI5351_REGISTER_17_CLK1_CONTROL            = const(17)
_SI5351_REGISTER_18_CLK2_CONTROL            = const(18)
_SI5351_REGISTER_19_CLK3_CONTROL            = const(19)
_SI5351_REGISTER_20_CLK4_CONTROL            = const(20)
_SI5351_REGISTER_21_CLK5_CONTROL            = const(21)
_SI5351_REGISTER_22_CLK6_CONTROL            = const(22)
_SI5351_REGISTER_23_CLK7_CONTROL            = const(23)
_SI5351_REGISTER_24_CLK3_0_DISABLE_STATE    = const(24)
_SI5351_REGISTER_25_CLK7_4_DISABLE_STATE    = const(25)
_SI5351_REGISTER_42_MULTISYNTH0_PARAMETERS_1 = const(42)
_SI5351_REGISTER_43_MULTISYNTH0_PARAMETERS_2 = const(43)
_SI5351_REGISTER_44_MULTISYNTH0_PARAMETERS_3 = const(44)
_SI5351_REGISTER_45_MULTISYNTH0_PARAMETERS_4 = const(45)
_SI5351_REGISTER_46_MULTISYNTH0_PARAMETERS_5 = const(46)
_SI5351_REGISTER_47_MULTISYNTH0_PARAMETERS_6 = const(47)
_SI5351_REGISTER_48_MULTISYNTH0_PARAMETERS_7 = const(48)
_SI5351_REGISTER_49_MULTISYNTH0_PARAMETERS_8 = const(49)
_SI5351_REGISTER_50_MULTISYNTH1_PARAMETERS_1 = const(50)
_SI5351_REGISTER_51_MULTISYNTH1_PARAMETERS_2 = const(51)
_SI5351_REGISTER_52_MULTISYNTH1_PARAMETERS_3 = const(52)
_SI5351_REGISTER_53_MULTISYNTH1_PARAMETERS_4 = const(53)
_SI5351_REGISTER_54_MULTISYNTH1_PARAMETERS_5 = const(54)
_SI5351_REGISTER_55_MULTISYNTH1_PARAMETERS_6 = const(55)
_SI5351_REGISTER_56_MULTISYNTH1_PARAMETERS_7 = const(56)
_SI5351_REGISTER_57_MULTISYNTH1_PARAMETERS_8 = const(57)
_SI5351_REGISTER_58_MULTISYNTH2_PARAMETERS_1 = const(58)
_SI5351_REGISTER_59_MULTISYNTH2_PARAMETERS_2 = const(59)
_SI5351_REGISTER_60_MULTISYNTH2_PARAMETERS_3 = const(60)
_SI5351_REGISTER_61_MULTISYNTH2_PARAMETERS_4 = const(61)
_SI5351_REGISTER_62_MULTISYNTH2_PARAMETERS_5 = const(62)
_SI5351_REGISTER_63_MULTISYNTH2_PARAMETERS_6 = const(63)
_SI5351_REGISTER_64_MULTISYNTH2_PARAMETERS_7 = const(64)
_SI5351_REGISTER_65_MULTISYNTH2_PARAMETERS_8 = const(65)
_SI5351_REGISTER_66_MULTISYNTH3_PARAMETERS_1 = const(66)
_SI5351_REGISTER_67_MULTISYNTH3_PARAMETERS_2 = const(67)
_SI5351_REGISTER_68_MULTISYNTH3_PARAMETERS_3 = const(68)
_SI5351_REGISTER_69_MULTISYNTH3_PARAMETERS_4 = const(69)
_SI5351_REGISTER_70_MULTISYNTH3_PARAMETERS_5 = const(70)
_SI5351_REGISTER_71_MULTISYNTH3_PARAMETERS_6 = const(71)
_SI5351_REGISTER_72_MULTISYNTH3_PARAMETERS_7 = const(72)
_SI5351_REGISTER_73_MULTISYNTH3_PARAMETERS_8 = const(73)
_SI5351_REGISTER_74_MULTISYNTH4_PARAMETERS_1 = const(74)
_SI5351_REGISTER_75_MULTISYNTH4_PARAMETERS_2 = const(75)
_SI5351_REGISTER_76_MULTISYNTH4_PARAMETERS_3 = const(76)
_SI5351_REGISTER_77_MULTISYNTH4_PARAMETERS_4 = const(77)
_SI5351_REGISTER_78_MULTISYNTH4_PARAMETERS_5 = const(78)
_SI5351_REGISTER_79_MULTISYNTH4_PARAMETERS_6 = const(79)
_SI5351_REGISTER_80_MULTISYNTH4_PARAMETERS_7 = const(80)
_SI5351_REGISTER_81_MULTISYNTH4_PARAMETERS_8 = const(81)
_SI5351_REGISTER_82_MULTISYNTH5_PARAMETERS_1 = const(82)
_SI5351_REGISTER_83_MULTISYNTH5_PARAMETERS_2 = const(83)
_SI5351_REGISTER_84_MULTISYNTH5_PARAMETERS_3 = const(84)
_SI5351_REGISTER_85_MULTISYNTH5_PARAMETERS_4 = const(85)
_SI5351_REGISTER_86_MULTISYNTH5_PARAMETERS_5 = const(86)
_SI5351_REGISTER_87_MULTISYNTH5_PARAMETERS_6 = const(87)
_SI5351_REGISTER_88_MULTISYNTH5_PARAMETERS_7 = const(88)
_SI5351_REGISTER_89_MULTISYNTH5_PARAMETERS_8 = const(89)
_SI5351_REGISTER_90_MULTISYNTH6_PARAMETERS   = const(90)
_SI5351_REGISTER_91_MULTISYNTH7_PARAMETERS   = const(91)
_SI5351_REGISTER_092_CLOCK_6_7_OUTPUT_DIVIDER = const(92)
_SI5351_REGISTER_165_CLK0_INITIAL_PHASE_OFFSET = const(165)
_SI5351_REGISTER_166_CLK1_INITIAL_PHASE_OFFSET = const(166)
_SI5351_REGISTER_167_CLK2_INITIAL_PHASE_OFFSET = const(167)
_SI5351_REGISTER_168_CLK3_INITIAL_PHASE_OFFSET = const(168)
_SI5351_REGISTER_169_CLK4_INITIAL_PHASE_OFFSET = const(169)
_SI5351_REGISTER_170_CLK5_INITIAL_PHASE_OFFSET = const(170)
_SI5351_REGISTER_177_PLL_RESET              = const(177)

_SI5351_CLK_DRIVE_STRENGTH_MASK = const(3 << 0)
STRENGTH_2MA = const(0 << 0)
STRENGTH_4MA = const(1 << 0)
STRENGTH_6MA = const(2 << 0)
STRENGTH_8MA = const(3 << 0)

_SI5351_CRYSTAL_LOAD      = const(183)
_SI5351_CRYSTAL_LOAD_MASK = const(3 << 6)
SI5351_CRYSTAL_LOAD_0PF   = const(0 << 6)
SI5351_CRYSTAL_LOAD_6PF   = const(1 << 6)
SI5351_CRYSTAL_LOAD_8PF   = const(2 << 6)
SI5351_CRYSTAL_LOAD_10PF  = const(3 << 6)

SI5351_FREQ_MULT             = const(100)
SI5351_MULTISYNTH_DIVBY4_FREQ = const(150000000)

R_DIV_1   = 0
R_DIV_2   = 1
R_DIV_4   = 2
R_DIV_8   = 3
R_DIV_16  = 4
R_DIV_32  = 5
R_DIV_64  = 6
R_DIV_128 = 7


class SI5351:
    """SI5351 / MS5351M clock generator driver for MicroPython.

    Parameters
    ----------
    data         : machine.Pin -- SDA pin
    clock        : machine.Pin -- SCL pin
    addr         : int         -- I2C address (default 0x60, ADDR pin low)
    crystal_freq : float       -- Reference oscillator frequency in Hz.
                                  For the Intro-to-CAD-2026 board use 24_576_000.0.
                                  For Cadyn's Pico SDR use 25_000_700.02 (calibrated).
    """

    # ------------------------------------------------------------------
    # Inner class: PLL (PLLA or PLLB)
    # ------------------------------------------------------------------
    class _PLL:
        def __init__(self, si5351, base_address, clock_control_enabled):
            self._si5351 = si5351
            self._base = base_address
            self._frequency = None
            self.clock_control_enabled = clock_control_enabled

        @property
        def frequency(self):
            return self._frequency

        def _configure_registers(self, p1, p2, p3):
            self._si5351._write_u8(self._base,     (p3 & 0x0000FF00) >> 8)
            self._si5351._write_u8(self._base + 1, (p3 & 0x000000FF))
            self._si5351._write_u8(self._base + 2, (p1 & 0x00030000) >> 16)
            self._si5351._write_u8(self._base + 3, (p1 & 0x0000FF00) >> 8)
            self._si5351._write_u8(self._base + 4, (p1 & 0x000000FF))
            self._si5351._write_u8(self._base + 5,
                ((p3 & 0x000F0000) >> 12) | ((p2 & 0x000F0000) >> 16))
            self._si5351._write_u8(self._base + 6, (p2 & 0x0000FF00) >> 8)
            self._si5351._write_u8(self._base + 7, (p2 & 0x000000FF))

        def _configure_registers_bulk(self, p1, p2, p3):
            buf = bytearray(9)
            buf[0] = self._base
            buf[1] = (p3 & 0x0000FF00) >> 8
            buf[2] = (p3 & 0x000000FF)
            buf[3] = (p1 & 0x00030000) >> 16
            buf[4] = (p1 & 0x0000FF00) >> 8
            buf[5] = (p1 & 0x000000FF)
            buf[6] = ((p3 & 0x000F0000) >> 12) | ((p2 & 0x000F0000) >> 16)
            buf[7] = (p2 & 0x0000FF00) >> 8
            buf[8] = (p2 & 0x000000FF)
            self._si5351._write_bulk(buf)

        def configure_integer(self, multiplier):
            """Set PLL with a purely integer feedback multiplier (15-90).
            Produces the cleanest VCO but coarser frequency steps.
            """
            assert 14 < multiplier < 91
            multiplier = int(multiplier)
            p1 = 128 * multiplier - 512
            p2 = 0
            p3 = 1
            self._configure_registers(p1, p2, p3)
            # Use instance crystal_freq so the reported frequency is accurate.
            self._frequency = self._si5351._crystal_freq * multiplier

        def configure_fractional(self, multiplier, numerator, denominator):
            """Set PLL with a fractional feedback multiplier.
            Allows precise frequency placement at any point in the HF range.
            Required when the desired output frequency is not an exact integer
            multiple of (crystal_freq / output_divider).
            """
            assert 14 < multiplier < 91
            assert 0 < denominator <= 0xFFFFF
            assert 0 <= numerator < 0xFFFFF
            multiplier  = int(multiplier)
            numerator   = int(numerator)
            denominator = int(denominator)
            p1 = int(128 * multiplier
                     + math.floor(128 * (numerator / denominator)) - 512)
            p2 = int(128 * numerator
                     - denominator * math.floor(128 * (numerator / denominator)))
            p3 = denominator
            self._configure_registers_bulk(p1, p2, p3)
            self._frequency = (self._si5351._crystal_freq
                               * (multiplier + numerator / denominator))

    # ------------------------------------------------------------------
    # Inner class: Clock output (CLK0, CLK1, CLK2)
    # ------------------------------------------------------------------
    class _Clock:
        def __init__(self, si5351, base_address, control_register,
                     r_register, phase):
            self._si5351    = si5351
            self._base      = base_address
            self._control   = control_register
            self._r         = r_register
            self._phase     = phase
            self._pll       = None
            self._divider   = None
            self.olddivider = 0

        @property
        def frequency(self):
            if self._pll is None or self._divider is None:
                return None
            base_frequency = self._pll.frequency / self._divider
            r = self.r_divider
            if   r == R_DIV_1:   return base_frequency
            elif r == R_DIV_2:   return base_frequency / 2
            elif r == R_DIV_4:   return base_frequency / 4
            elif r == R_DIV_8:   return base_frequency / 8
            elif r == R_DIV_16:  return base_frequency / 16
            elif r == R_DIV_32:  return base_frequency / 32
            elif r == R_DIV_64:  return base_frequency / 64
            elif r == R_DIV_128: return base_frequency / 128
            else: raise RuntimeError("Unexpected R divider!")

        @property
        def r_divider(self):
            return (self._si5351._read_u8(self._r) >> 4) & 0x07

        @r_divider.setter
        def r_divider(self, divider):
            assert 0 <= divider <= 7
            reg_value  = self._si5351._read_u8(self._r)
            reg_value &= 0x0F
            reg_value |= (divider & 0x07) << 4
            self._si5351._write_u8(self._r, reg_value)

        def drive_strength(self, s):
            control  = self._si5351._read_u8(self._control)
            control &= ~(_SI5351_CLK_DRIVE_STRENGTH_MASK)
            control |= s
            self._si5351._write_u8(self._control, control)

        def _configure_registers(self, p1, p2, p3):
            self._si5351._write_u8(self._base,     (p3 & 0x0000FF00) >> 8)
            self._si5351._write_u8(self._base + 1, (p3 & 0x000000FF))
            self._si5351._write_u8(self._base + 2, (p1 & 0x00030000) >> 16)
            self._si5351._write_u8(self._base + 3, (p1 & 0x0000FF00) >> 8)
            self._si5351._write_u8(self._base + 4, (p1 & 0x000000FF))
            self._si5351._write_u8(self._base + 5,
                ((p3 & 0x000F0000) >> 12) | ((p2 & 0x000F0000) >> 16))
            self._si5351._write_u8(self._base + 6, (p2 & 0x0000FF00) >> 8)
            self._si5351._write_u8(self._base + 7, (p2 & 0x000000FF))

        def configure_integer(self, pll, divider):
            """Set the output Multisynth to an integer divider.
            This is REQUIRED for quadrature phase offset to work correctly.
            The 90° phase shift relies on PHOFF = divider (see module docstring).
            """
            if divider == self.olddivider:
                return
            assert 3 < divider < 901
            divider = int(divider)
            assert pll.frequency is not None
            p1 = 128 * divider - 512
            p2 = 0
            p3 = 1
            self._configure_registers(p1, p2, p3)
            control  = self._si5351._read_u8(self._control)
            control |= pll.clock_control_enabled
            control |= 1 << 6          # integer mode bit
            self._si5351._write_u8(self._control, control)
            self._pll         = pll
            self._divider     = divider
            self.olddivider   = divider

        def configure_fractional(self, pll, divider, numerator, denominator):
            assert 3 < divider < 901
            assert 0 < denominator <= 0xFFFFF
            assert 0 <= numerator < 0xFFFFF
            divider     = int(divider)
            numerator   = int(numerator)
            denominator = int(denominator)
            assert pll.frequency is not None
            p1 = int(128 * divider
                     + math.floor(128 * (numerator / denominator)) - 512)
            p2 = int(128 * numerator
                     - denominator * math.floor(128 * (numerator / denominator)))
            p3 = denominator
            self._configure_registers(p1, p2, p3)
            control  = self._si5351._read_u8(self._control)
            control |= pll.clock_control_enabled
            self._si5351._write_u8(self._control, control)
            self._pll     = pll
            self._divider = divider + (numerator / denominator)

        @property
        def phase(self):
            return self._phase

    # ------------------------------------------------------------------
    # Class-level I/O buffers (not thread-safe)
    # ------------------------------------------------------------------
    _BUFFER      = bytearray(2)
    _READ_BUFFER = bytearray(1)

    # ------------------------------------------------------------------
    # Constructor
    # ------------------------------------------------------------------
    def __init__(self, data, clock, *, addr=_SI5351_ADDRESS,
                 crystal_freq=_SI5351_CRYSTAL_FREQUENCY_DEFAULT):
        """
        Parameters
        ----------
        data         : Pin  -- SDA
        clock        : Pin  -- SCL
        addr         : int  -- I2C address (0x60 when ADDR pin is low)
        crystal_freq : float -- Reference oscillator frequency in Hz.
                                Intro-to-CAD-2026: 24_576_000.0
                                Cadyn's Pico SDR:  25_000_700.02 (calibrated)
        """
        self.i2c_addr    = addr
        self._crystal_freq = float(crystal_freq)
        # GPIO12/GPIO13 → I2C0 on RP2040 (same bus as GPIO4/GPIO5)
        # Use lower I2C frequency for better reliability
        self.i2c = I2C(0, freq=100_000, scl=clock, sda=data)
        self.oldmult = 0
        # Updated by set_frequency() so callers can display PLL status.
        # 'G' = integer PLL (golden), 'F' = fractional, 'X' = fallback.
        self._last_pll_type = 'X'
        self._last_quality  = 0     # offset_hz for 'G', score 0-0.5 for 'F'

        # Wait for the Si5351 to signal that its power-on initialisation is
        # complete.  Register 0 bit 7 (SYS_INIT) = 1 while the chip is still
        # initialising; when it clears to 0 the device is ready.
        retry_count = 0
        max_retries = 5  # Reduced retries since I2C is working
        while retry_count < max_retries:
            try:
                status = self._read_u8(_SI5351_REGISTER_0_DEVICE_STATUS)
                if (status >> 7) == 0:
                    break
                print(f"Si5351 initializing... status: {status:02X}")
            except OSError as e:
                print(f"I2C error during init: {e}")
            retry_count += 1
            if retry_count < max_retries:
                time.sleep_ms(200)  # Wait 200ms between retries
        
        if retry_count >= max_retries:
            print("Warning: Si5351 may not be fully initialized, continuing anyway")

        # Disable all outputs
        self._write_u8(_SI5351_REGISTER_3_OUTPUT_ENABLE_CONTROL, 0xFF)
        # Crystal load: 8 pF (matches MS5351M / Si5351A-B-GT default)
        self._write_u8(_SI5351_CRYSTAL_LOAD,
                       (SI5351_CRYSTAL_LOAD_8PF & _SI5351_CRYSTAL_LOAD_MASK)
                       | 0b00010010)

        # Power down all output drivers
        for reg in range(16, 24):
            self._write_u8(reg, 0x80)

        # Turn the clock outputs back on (no source selected yet)
        for reg in range(16, 24):
            self._write_u8(reg, 0x0C)

        # PLL objects
        self.pll_a = self._PLL(self, 26, 0)
        self.pll_b = self._PLL(self, 34, (1 << 5))

        # Clock output objects
        self.clock_0 = self._Clock(
            self,
            _SI5351_REGISTER_42_MULTISYNTH0_PARAMETERS_1,
            _SI5351_REGISTER_16_CLK0_CONTROL,
            _SI5351_REGISTER_44_MULTISYNTH0_PARAMETERS_3,
            _SI5351_REGISTER_165_CLK0_INITIAL_PHASE_OFFSET,
        )
        self.clock_1 = self._Clock(
            self,
            _SI5351_REGISTER_50_MULTISYNTH1_PARAMETERS_1,
            _SI5351_REGISTER_17_CLK1_CONTROL,
            _SI5351_REGISTER_52_MULTISYNTH1_PARAMETERS_3,
            _SI5351_REGISTER_166_CLK1_INITIAL_PHASE_OFFSET,
        )
        self.clock_2 = self._Clock(
            self,
            _SI5351_REGISTER_58_MULTISYNTH2_PARAMETERS_1,
            _SI5351_REGISTER_18_CLK2_CONTROL,
            _SI5351_REGISTER_60_MULTISYNTH2_PARAMETERS_3,
            _SI5351_REGISTER_167_CLK2_INITIAL_PHASE_OFFSET,
        )

    # ------------------------------------------------------------------
    # Frequency and phase control
    # ------------------------------------------------------------------
    def set_frequency(self, freq, clock, pll, mult, half_bw=24_000):
        """Set a clock output to freq Hz.

        Returns N actually programmed.  Caller passes to set_phase() for 90°
        quadrature (PHOFF = N requires N to be a plain integer).

        Selection strategy
        ------------------
        For every valid output divider N (N_min..127 where VCO stays 600-900 MHz):
          M_exact = freq * N / F_ref
          frac(M) = M_exact - floor(M_exact)
          score   = min(frac(M), 1 - frac(M))   # 0 = integer (best), 0.5 = worst

        Priority 1 — Integer PLL within half_bw:
          If round(M_exact) is integer and |f_int - freq| <= half_bw, track the
          candidate with the smallest offset.  Integer PLL means zero fractional
          spurs; snapping to it is free when the LO stays inside the audio band.

        Priority 2 — Best fractional PLL (lowest score):
          Among all N, choose the one whose frac(M) is closest to an integer.
          This minimises fractional spur energy while hitting exactly freq.

        Fallback — freq below ~4.7 MHz (VCO out of spec):
          Use the band multiplier `mult` passed by radio.py.

        half_bw should equal sample_rate / 2 (24 000 Hz for a 48 kHz card).
        The RATE serial command updates it dynamically from Quisk.
        """
        N_min = max(4,   int(math.ceil(600_000_000.0 / freq)))
        N_max = min(127, int(900_000_000.0 / freq))

        best_int  = None   # (offset, N, M_integer)
        best_frac = None   # (score,  N, M_exact)

        for N in range(N_min, N_max + 1):
            M_exact = freq * N / self._crystal_freq
            if not (14 < M_exact < 91):
                continue
            frac_part = M_exact - math.floor(M_exact)
            score     = frac_part if frac_part <= 0.5 else 1.0 - frac_part

            # Check integer-PLL candidate
            M_int  = round(M_exact)
            f_int  = self._crystal_freq * M_int / N
            offset = abs(freq - f_int)
            if offset <= half_bw:
                if best_int is None or offset < best_int[0]:
                    best_int = (offset, N, M_int)

            # Track best fractional candidate
            if best_frac is None or score < best_frac[0]:
                best_frac = (score, N, M_exact)

        if best_int is not None:
            N, M_int = best_int[1], best_int[2]
            f_int = self._crystal_freq * M_int / N
            self._last_pll_type = 'G'
            self._last_quality  = int(f_int - freq)   # signed: + means LO above target
            pll.configure_integer(M_int)
            clock.configure_integer(pll, N)
            return N

        if best_frac is not None:
            self._last_pll_type = 'F'
            self._last_quality  = best_frac[0]
            N       = best_frac[1]
            M_exact = best_frac[2]
            intpart = math.floor(M_exact)
            denom   = 1_000_000
            numer   = (M_exact - intpart) * denom
            pll.configure_fractional(intpart, numer, denom)
            clock.configure_integer(pll, N)
            return N

        # Fallback: freq below ~4.7 MHz, VCO out of spec
        self._last_pll_type = 'X'
        self._last_quality  = 0
        pll_freq   = freq * mult
        multiplier = pll_freq / self._crystal_freq
        intpart    = math.floor(multiplier)
        denom      = 1_000_000
        numer      = (multiplier - intpart) * denom
        pll.configure_fractional(intpart, numer, denom)
        clock.configure_integer(pll, mult)
        return mult

    def set_phase(self, clock, pll, phase):
        """Write the PHOFF register for the given clock and reset both PLLs.

        For 90° quadrature: call with phase = mult (the same integer output
        divider used in set_frequency).  The chip interprets PHOFF in units
        of 1/4 VCO period, so PHOFF = mult → mult × (1/4 VCO period)
        = 1/4 output period = 90°.
        """
        phase = phase & 0b01111111
        self._write_u8(clock.phase, phase)
        # Simultaneously reset both PLLs so CLK0 and CLK1 re-lock in phase.
        self._write_u8(_SI5351_REGISTER_177_PLL_RESET, (1 << 7) | (1 << 5))
        self.outputs_enabled = True

    # ------------------------------------------------------------------
    # Low-level I2C helpers
    # ------------------------------------------------------------------
    def _read_u8(self, register):
        self._BUFFER[0] = register & 0xFF
        self.i2c.writeto(self.i2c_addr, self._BUFFER)
        self.i2c.readfrom_into(self.i2c_addr, self._READ_BUFFER)
        return self._READ_BUFFER[0]

    def _write_u8(self, register, val):
        self._BUFFER[0] = register & 0xFF
        self._BUFFER[1] = val & 0xFF
        try:
            self.i2c.writeto(self.i2c_addr, self._BUFFER)
        except OSError as e:
            print(f"I2C write error to register {register:02X}: {e}")
            raise

    def _write_bulk(self, buf):
        self.i2c.writeto(self.i2c_addr, buf)

    # ------------------------------------------------------------------
    # Output enable
    # ------------------------------------------------------------------
    @property
    def outputs_enabled(self):
        return self._read_u8(_SI5351_REGISTER_3_OUTPUT_ENABLE_CONTROL) == 0x00

    @outputs_enabled.setter
    def outputs_enabled(self, val):
        if val:
            self._write_u8(_SI5351_REGISTER_3_OUTPUT_ENABLE_CONTROL, 0x00)
        else:
            self._write_u8(_SI5351_REGISTER_3_OUTPUT_ENABLE_CONTROL, 0xFF)
