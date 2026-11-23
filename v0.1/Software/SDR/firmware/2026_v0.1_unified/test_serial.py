import serial, time

ser = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
ser.write(b"WIFI?\r\n")
time.sleep(0.1)
print(ser.read_all().decode(errors='replace'))
