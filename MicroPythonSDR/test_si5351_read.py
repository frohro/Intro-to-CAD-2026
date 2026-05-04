# Test direct I2C communication with Si5351
print("Testing Si5351 I2C communication...")

try:
    from machine import Pin, I2C
    import config
    print("Imports successful")
    
    # Create I2C instance
    i2c = I2C(0, freq=100_000, scl=Pin(config.SCL_PIN), sda=Pin(config.SDA_PIN))
    print("I2C instance created")
    
    # Test reading device status register (register 0)
    print("Reading device status register...")
    try:
        i2c.writeto(0x60, b'\x00')  # Write register address
        data = i2c.readfrom(0x60, 1)  # Read 1 byte
        status = data[0]
        print(f"Device status: 0x{status:02X} (binary: {status:08b})")
        
        # Check SYS_INIT bit (bit 7)
        sys_init = (status >> 7) & 1
        print(f"SYS_INIT bit: {sys_init} (0=ready, 1=initializing)")
        
        # Check LOS bit (bit 6) - Loss of Signal
        los = (status >> 6) & 1
        print(f"LOS bit: {los} (0=signal OK, 1=loss of signal)")
        
        # Check LOL bit (bit 5) - Loss of Lock
        lol = (status >> 5) & 1
        print(f"LOL bit: {lol} (0=locked, 1=loss of lock)")
        
    except Exception as e:
        print(f"Error reading status register: {e}")
    
    # Test writing and reading a simple register
    print("\nTesting register write/read...")
    try:
        # Read current value of register 183 (CLK1 control)
        i2c.writeto(0x60, b'\xB7')  # Register 183
        data = i2c.readfrom(0x60, 1)
        print(f"Register 183 (CLK1 control): 0x{data[0]:02X}")
    except Exception as e:
        print(f"Error reading register 183: {e}")
        
except Exception as e:
    print(f"Test failed: {e}")
    import sys
    sys.print_exception(e)

print("Test complete")
