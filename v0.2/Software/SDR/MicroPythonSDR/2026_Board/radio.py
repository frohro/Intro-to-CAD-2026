import machine
import time
import math
from micropython import const
import si5351
import config

try:
    import neopixel as _neopixel
    _NEOPIXEL_OK = True
except ImportError:
    _NEOPIXEL_OK = False

try:
    import ssd1306
    _OLED_OK = True
except ImportError:
    _OLED_OK = False

class Radio:

    MIN_FREQUENCY = const( 3800000 )
    MAX_FREQUENCY = const( 30000000 )

    def __init__(self, s, frequency, sample_rate=48_000):
        self._si5351 = s
        self._frequency = frequency
        self._mult = 100
        self._old_mult = 0
        self._half_bw = sample_rate // 2
        self._pll_type = 'X'
        self._quality  = 0
        self._si5351.clock_0.drive_strength( si5351.STRENGTH_8MA )
        self._si5351.clock_1.drive_strength( si5351.STRENGTH_8MA )
        # NeoPixel status LED (WS2812B on GPIO23 of the YD-RP2040 V1.3).
        # Green = integer PLL (golden), yellow = good frac, red = poor frac.
        self._led = None
        led_pin = getattr(config, 'LED_PIN', None)
        if _NEOPIXEL_OK and led_pin is not None:
            try:
                self._led = _neopixel.NeoPixel(machine.Pin(led_pin), 1)
                self._set_led('X', 0)   # dim purple = initialising
            except Exception:
                self._led = None

        # SSD1306 OLED display on I2C bus (same as Si5351)
        self._oled = None
        if _OLED_OK:
            try:
                # Use the same I2C bus as Si5351
                i2c = self._si5351.i2c
                OLED_ADDR = 0x3C
                if OLED_ADDR in i2c.scan():
                    self._oled = ssd1306.SSD1306_I2C(128, 64, i2c, addr=OLED_ADDR)
                    self._oled.fill(0)
                    self._oled.show()
            except Exception:
                self._oled = None

    def _set_led(self, pll_type, quality):
        """Update the NeoPixel colour to indicate the current PLL mode.

        Green  — integer PLL (golden frequency, no fractional spurs)
        Yellow — fractional PLL, low spur score (quality < 0.1)
        Red    — fractional PLL, high spur score (quality >= 0.1)
        Purple — fallback / unknown
        """
        if self._led is None:
            return
        if pll_type == 'G':
            color = (0, 32, 0)                # green
        elif pll_type == 'F' and quality < 0.1:
            color = (24, 24, 0)               # yellow
        elif pll_type == 'F':
            color = (32, 0, 0)                # red
        else:
            color = (16, 0, 16)               # purple
        self._led[0] = color
        self._led.write()

    def _update_oled(self, freq, pll_type, n):
        """Update the OLED display with frequency and PLL parameters."""
        if self._oled is None:
            return
        self._oled.fill(0)
        self._oled.text("Freq: {:.3f} MHz".format(freq / 1_000_000), 0, 0)
        M = self._si5351.pll_a.frequency / self._si5351._crystal_freq
        self._oled.text("M: {:.2f}, N: {}".format(M, n), 0, 16)
        self._oled.show()

    def setFrequency(self):
        f = self._frequency
        if f < 8000000:
            self._mult = 100
        elif f < 11000000:
            self._mult = 80
        elif f < 15000000:
            self._mult = 50
        else:
            self._mult = 30

        # Quisk now chooses the exact LO to program (golden or fractional).
        # Pass half_bw=0 so set_frequency treats any integer solution as valid
        # (it never snaps since Quisk already sent the exact target).
        # The fractional fallback still kicks in for out-of-spec VCO.
        n = self._si5351.set_frequency( self._frequency, self._si5351.clock_0, self._si5351.pll_a, self._mult, 0 )
        self._si5351.set_frequency( self._frequency, self._si5351.clock_1, self._si5351.pll_a, self._mult, 0 )

        # Only reset the PLLs (and write PHOFF) when N changes, to avoid
        # unnecessary re-lock glitches on every frequency step.
        if n != self._old_mult:
            self._si5351.set_phase( self._si5351.clock_1, self._si5351.pll_a, n )
        self._old_mult = n
        
        # CRITICAL: Enable outputs after configuration
        self._si5351.outputs_enabled = True
        
        # Read PLL status written by set_frequency(), update LED and cache
        # for the serial FREQ response.
        self._pll_type = self._si5351._last_pll_type
        self._quality  = self._si5351._last_quality
        self._set_led(self._pll_type, self._quality)
        self._update_oled(self._frequency, self._pll_type, n)
        
        # Debug output
        print(f"DEBUG: Freq={f}Hz, Mult={self._mult}, PLL={self._pll_type}, Outputs={self._si5351.outputs_enabled}")

    def _pll_status_line(self):
        """Return the status portion of the FREQ response: OK,type,signed_offset.

        signed_offset = f_lo - f_requested (Hz).
        Positive: LO above tune, DC spike appears below tune in audio.
        Negative: LO below tune, DC spike appears above tune in audio.
        For fractional PLL the offset is always 0 (exact frequency).
        """
        if self._pll_type == 'G':
            return "OK,G,{}".format(self._quality)   # signed int Hz
        elif self._pll_type == 'F':
            return "OK,F,0"
        else:
            return "OK,X,0"

    def run(self):
        self.setFrequency()
        print("SDR ready - waiting for commands...")
        while True:
            try:
                user_input = input()
                if user_input == "FREQ,":
                    print(self._frequency)
                    self.setFrequency()
                    print(self._pll_status_line())
                elif user_input.startswith("FREQ,"):
                    try:
                        self._frequency = int(user_input[5:13])
                        print(self._frequency)
                        self.setFrequency()
                        print(self._pll_status_line())
                    except ValueError:
                        print("Error: Invalid frequency")
                elif user_input.startswith("RATE,"):
                    try:
                        self._half_bw = int(user_input[5:]) // 2
                        print("OK")
                    except ValueError:
                        print("Error: Invalid sample rate")
                elif user_input.startswith("XTAL"):
                    # Return crystal reference frequency so Quisk can run the
                    # golden-frequency search on the PC side.
                    print(self._si5351._crystal_freq)
                    print("OK")
                elif user_input.startswith("VER"):
                    print("SDR Pi Pico version 0.1")
                    print("OK")
                time.sleep(0.05)
            except Exception as e:
                print(f"Error in main loop: {e}")
                time.sleep(0.1)  # Brief pause before retrying
