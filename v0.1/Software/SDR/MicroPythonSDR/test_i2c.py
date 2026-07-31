import time
from machine import Pin, I2C
import config

SI5351_ADDR = 0x60

print("Testing I2C...")

try:
    # Create I2C instance
    i2c = I2C(0, freq=100_000, scl=Pin(config.SCL_PIN), sda=Pin(config.SDA_PIN))
    
    # 1. Scan for devices
    devices = i2c.scan()
    if SI5351_ADDR not in devices:
        raise Exception(f"Si5351 not found at 0x{SI5351_ADDR:02X}")
    print(f"Si5351 found at 0x{SI5351_ADDR:02X}")

    # 2. Test strictly compliant I2C Read (Polling SYS_INIT)
    # readfrom_mem uses the correct Repeated Start protocol
    ready = False
    for _ in range(10):
        status = i2c.readfrom_mem(SI5351_ADDR, 0x00, 1)[0]
        if (status >> 7) == 0:  # Check if SYS_INIT bit (bit 7) is cleared
            ready = True
            break
        time.sleep(0.01)
        
    if ready:
        print("Si5351 initialized and ready.")
    else:
        print("Warning: Si5351 SYS_INIT stuck high.")

    # 3. Test True I2C Read AND Write
    # We will test using Register 3 (Output Enable Control)
    # 1 = Output Disabled, 0 = Output Enabled
    
    # Step A: Read original state
    orig_reg3 = i2c.readfrom_mem(SI5351_ADDR, 0x03, 1)[0]
    print(f"Original Reg 3: 0x{orig_reg3:02X}")
    
    # Step B: Write an inverted/test state (e.g., disable all outputs by writing 0xFF)
    test_val = 0xFF if orig_reg3 != 0xFF else 0xFE
    i2c.writeto_mem(SI5351_ADDR, 0x03, bytes([test_val]))
    
    # Step C: Read back to verify the write worked
    new_reg3 = i2c.readfrom_mem(SI5351_ADDR, 0x03, 1)[0]
    if new_reg3 == test_val:
        print(f"Write test PASSED! Reg 3 updated to: 0x{new_reg3:02X}")
    else:
        print(f"Write test FAILED! Expected 0x{test_val:02X}, got 0x{new_reg3:02X}")
        
    # Step D: Restore original state to leave the chip clean
    i2c.writeto_mem(SI5351_ADDR, 0x03, bytes([orig_reg3]))
    print("State restored. Test complete.")

except Exception as e:
    print(f"I2C test failed: {e}")