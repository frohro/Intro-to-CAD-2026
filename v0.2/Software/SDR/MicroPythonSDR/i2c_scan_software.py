import machine

# 1. Initialize Hardware I2C
# I2C(0) refers to the first hardware I2C controller on the Pico
# Default for I2C(0) is SDA=Pin(4), SCL=Pin(5)
i2c = machine.I2C(0, scl=machine.Pin(13), sda=machine.Pin(12), freq=100000)

print("--- I2C Bus Scanner ---")

# 2. Perform the scan
try:
    devices = i2c.scan()

    if not devices:
        print("No I2C devices found. Check your wiring and pull-up resistors.")
    else:
        print(f"Scan successful! Found {len(devices)} device(s):")
        for device in devices:
            # Print in both decimal and hex (standard for datasheets)
            print(f"  -> Device found at address: {hex(device)} (Decimal: {device})")

except Exception as e:
    print(f"An error occurred: {e}")

print("-----------------------")
