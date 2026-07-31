# Basic test to verify MicroPython is working
print("Hello World!")

# Test basic imports
try:
    import machine
    print("machine imported")
except Exception as e:
    print(f"machine import failed: {e}")

try:
    from machine import Pin
    print("Pin imported")
except Exception as e:
    print(f"Pin import failed: {e}")

# Test LED if available
try:
    import config
    led_pin = getattr(config, 'LED_PIN', None)
    if led_pin is not None:
        from machine import Pin
        led = Pin(led_pin, Pin.OUT)
        led.on()
        print("LED on")
except Exception as e:
    print(f"LED test failed: {e}")

print("Test complete")
