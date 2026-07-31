# Intro-to-CAD-2026 / Cadyn Pico SDR - MicroPython entry point
#
# Accepts Quisk serial commands over USB CDC:
#   FREQ,<hz>   - tune to frequency in Hz, responds with freq + "OK"
#   VER         - report firmware version, responds with version + "OK"
#
# To switch boards edit BOARD in config.py before deploying.

print("Starting SDR...")

try:
    print("Importing modules...")
    from machine import Pin
    from radio import Radio
    import si5351
    import config
    print("Modules imported successfully")
    
    print("Creating Si5351 instance...")
    synth = si5351.SI5351(
        data         = Pin(config.SDA_PIN),
        clock        = Pin(config.SCL_PIN),
        addr         = 0x60,
        crystal_freq = config.CRYSTAL_FREQ,
    )
    print("Si5351 created successfully")
    
    print("Creating Radio instance...")
    r = Radio(synth, config.DEFAULT_FREQ)
    print("Radio created successfully")
    
    print("Starting main loop...")
    r.run()
    
except Exception as e:
    print(f"Error: {e}")
    import sys
    print(f"Error type: {type(e)}")
    sys.print_exception(e)
