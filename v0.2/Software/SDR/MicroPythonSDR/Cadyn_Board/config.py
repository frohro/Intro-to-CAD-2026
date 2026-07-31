# Board configuration for MicroPythonSDR
#
# Change BOARD to match the hardware you are deploying to, then copy all
# .py files in this directory to the root of the Pico's filesystem.
#
# Supported boards
# ----------------
#   "INTRO_CAD_2026"  —  Intro-to-CAD-2026 SDR board (CPTR-480 class board)
#   "CADYN_PICO_SDR"  —  Cadyn's hand-wired Pico SDR (reference build)

BOARD = "CADYN_PICO_SDR"

# -----------------------------------------------------------------------
if BOARD == "INTRO_CAD_2026":
    # I2C bus 0: GPIO12 = SDA, GPIO13 = SCL  (RP2040 I2C0 alternate mapping)
    SDA_PIN      = 12
    SCL_PIN      = 13
    # MS5351M internal oscillator frequency.
    # Update this value after calibrating against a known-frequency signal
    # (e.g., WWV at 10 MHz, or a calibrated signal generator).
    CRYSTAL_FREQ = 24_576_000.0
    DEFAULT_FREQ = 7_074_000      # FT8 on 40 m
    LED_PIN      = 23             # WS2812B NeoPixel on YD-RP2040 V1.3

# -----------------------------------------------------------------------
elif BOARD == "CADYN_PICO_SDR":
    # I2C bus 0: GPIO4 = SDA, GPIO5 = SCL  (RP2040 I2C0 default mapping)
    SDA_PIN      = 4
    SCL_PIN      = 5
    # Calibrated crystal value from Cadyn's unit.
    CRYSTAL_FREQ = 25_000_700.02
    DEFAULT_FREQ = 7_000_000
    LED_PIN      = None           # no NeoPixel on this build

# -----------------------------------------------------------------------
else:
    raise ValueError("Unknown BOARD: " + BOARD)