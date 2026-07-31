#!/usr/bin/env python3
"""
test_multi.py -- Conformance test for unified Multi-ADC SDR firmware
                (Research/2026_v0.2_Multi/).

Verifies:
  1. CDC device found (VID:PID 0xcafe:0x4011)
  2. Quisk CDC protocol: VER, XTAL, MODE, ADC query/set, RATE query/set, FREQ
  3. UAC1 device enumeration: 'WWU SDR' in arecord -l
  4. USB descriptors: Three alternate settings supporting 48k S24, 96k S24, 192k S16
  5. Audio capture at all supported rates.

Usage:
    python3 test_multi.py
"""

import os
import re
import subprocess
import sys
import tempfile
import time

import serial
import serial.tools.list_ports

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"
WARN = "\033[33mWARN\033[0m"

failures = 0

def check(label, got, expected):
    global failures
    ok = (got == expected)
    status = PASS if ok else FAIL
    print(f"  {status}  {label}")
    print(f"         got      {got!r}")
    if not ok:
        print(f"         expected {expected!r}")
        failures += 1
    return ok


print("=" * 60)
print("Part 1 -- CDC / Serial port detection")
print("=" * 60)

KNOWN_VIDS_PIDS = [(0xcafe, 0x4011)]

port = None
port_device = None
found_vid, found_pid = None, None
for info in serial.tools.list_ports.comports():
    if (info.vid, info.pid) in KNOWN_VIDS_PIDS:
        try:
            port = serial.Serial(info.device, 115200, timeout=3)
            print(f"  Opened {info.device}  ({info.description})")
            found_vid, found_pid = info.vid, info.pid
            port_device = info.device
            break
        except serial.SerialException:
            continue

if port is None:
    for p in ("/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyACM2", "/dev/ttyACM3"):
        try:
            port = serial.Serial(p, 115200, timeout=3)
            print(f"  Opened {p}")
            found_vid, found_pid = 0xcafe, 0x4011
            port_device = p
            break
        except serial.SerialException:
            continue

if port is None:
    sys.exit("ERROR: No CDC device found (VID:PID 0xcafe:0x4011 or /dev/ttyACM0-3).")


print()
print("=" * 60)
print("Part 2 -- Quisk CDC protocol verification")
print("=" * 60)

def cmd(s, n=2):
    port.write((s + "\n").encode())
    return [port.readline().decode().rstrip("\r\n") for _ in range(n)]

print("\n-- Startup sentinel --")
port.write(b'\x03')
time.sleep(0.1)
port.write(b'\x04')

port.timeout = 0.2
deadline = time.time() + 6.0
got_ready = False
while time.time() < deadline:
    line = port.readline()
    if b'SDR ready' in line:
        got_ready = True
        print(f"  {PASS}  SDR ready received: {line.rstrip().decode()!r}")
        break
while port.readline():
    pass
port.timeout = 3

if not got_ready:
    print(f"  {FAIL}  SDR ready sentinel not received within 6 s")
    failures += 1

print("\n-- VER --")
r = cmd("VER")
check("line 1 starts with VER,", r[0][:4], "VER,")
check("line 2 = OK",              r[1],     "OK")

print("\n-- XTAL --")
r = cmd("XTAL")
check("line 1 = XTAL,24576000", r[0], "XTAL,24576000")
check("line 2 = OK",             r[1], "OK")

print("\n-- MODE --")
r = cmd("MODE")
check("line 1 = MODE,DIRECT", r[0], "MODE,DIRECT")
check("line 2 = OK",           r[1], "OK")

print("\n-- ADC selection --")
# Query default/current
r = cmd("ADC")
print(f"  Current ADC state: {r[0]}")
# Force PCM1808 for initial tests
cmd("ADC,PCM1808")
r = cmd("ADC")
check("ADC set to PCM1808", r[0], "ADC,PCM1808")
check("line 2 = OK",        r[1], "OK")

# Switch to CJC5430
r = cmd("ADC,CJC5430")
check("ADC,CJC5430 selection ack", r[0], "OK")

# Query updated
r = cmd("ADC")
check("updated ADC is CJC5430", r[0], "ADC,CJC5430")
check("line 2 = OK",            r[1], "OK")

# Switch back to PCM1808
r = cmd("ADC,PCM1808")
check("ADC,PCM1808 selection ack", r[0], "OK")

print("\n-- RATE selection --")
# Query default rate
r = cmd("RATE")
check("default rate is 48000", r[0], "RATE,48000")
check("line 2 = OK",           r[1], "OK")

# Set rate to 96000
r = cmd("RATE,96000")
check("RATE,96000 set ack", r[0], "OK")

r = cmd("RATE")
check("queried rate is 96000", r[0], "RATE,96000")
check("line 2 = OK",           r[1], "OK")

# Set rate to 192000 (forces CJC5430)
r = cmd("RATE,192000")
check("RATE,192000 set ack", r[0], "OK")

r = cmd("ADC")
check("ADC dynamically switched to CJC5430 at 192k", r[0], "ADC,CJC5430")

r = cmd("RATE")
check("queried rate is 192000", r[0], "RATE,192000")

# Restore defaults
cmd("ADC,PCM1808")
cmd("RATE,48000")

print("\n-- FREQ golden integer --")
r = cmd("FREQ,7168000,96,28,0,1,3072,0,1")
check("line 1 = 7168000", r[0], "7168000")
check("line 2 = OK,G,0",  r[1], "OK,G,0")

port.close()


print()
print("=" * 60)
print("Part 3 -- UAC1 audio device enumeration")
print("=" * 60)

result = subprocess.run(["arecord", "-l"], capture_output=True, text=True)
arecord_output = result.stdout + result.stderr

if "WWU SDR" in arecord_output:
    print(f"  {PASS}  'WWU SDR' found in arecord -l output")
else:
    print(f"  {FAIL}  'WWU SDR' NOT found in arecord -l")
    failures += 1

card_num = None
for line in arecord_output.splitlines():
    m = re.match(r"card (\d+):.*WWU SDR", line)
    if m:
        card_num = int(m.group(1))
        break


print()
print("=" * 60)
print("Part 4 -- USB descriptor verification")
print("=" * 60)

device_id = f"{found_vid:04x}:{found_pid:04x}"
lsusb_result = subprocess.run(
    ["lsusb", "-v", "-d", device_id],
    capture_output=True, text=True
)
lsusb_out = lsusb_result.stdout + lsusb_result.stderr

# Look for SubframeSizes
subframes = re.findall(r"bSubframeSize\s+(\d+)", lsusb_out)
check("Correct subframe size sequence (3 for 24-bit, 2 for 16-bit)", subframes, ["3", "3", "2"])

# Look for Resolutions
resolutions = re.findall(r"bBitResolution\s+(\d+)", lsusb_out)
check("Correct bit resolutions sequence (24, 24, 16)", resolutions, ["24", "24", "16"])

# Look for sampling frequencies advertised
check("48000 Hz advertised", "48000" in lsusb_out, True)
check("96000 Hz advertised", "96000" in lsusb_out, True)
check("192000 Hz advertised", "192000" in lsusb_out, True)


def check_audio(wav_path, min_size, bytes_per_sample):
    if not os.path.exists(wav_path):
        print(f"  {FAIL}  WAV file not created at {wav_path}")
        return False
    size = os.path.getsize(wav_path)
    if size >= min_size:
        print(f"  {PASS}  WAV file size {size} bytes (>= {min_size})")
    else:
        print(f"  {FAIL}  WAV file size {size} bytes (< {min_size})")
        return False

    with open(wav_path, "rb") as f:
        # Read from middle of file to check variation
        f.seek(44 + (min_size // 2))
        raw = f.read(1024)

    # Decode channels
    samples = []
    if bytes_per_sample == 3:
        # S24_3LE
        for j in range(0, len(raw) - 2, 3):
            v = raw[j] | (raw[j + 1] << 8) | (raw[j + 2] << 16)
            if v >= 0x800000:
                v -= 0x1000000
            samples.append(v)
    else:
        # S16_LE
        for j in range(0, len(raw) - 1, 2):
            v = raw[j] | (raw[j + 1] << 8)
            if v >= 0x8000:
                v -= 0x10000
            samples.append(v)

    if not samples:
        return False

    unique = len(set(samples))
    peak = max(abs(min(samples)), abs(max(samples)))

    if unique == 1 and samples[0] == 0:
        print(f"  {WARN}  Audio all-zero -- ADC not producing samples.")
    elif unique > 10 and peak > 10:
        print(f"  {PASS}  Audio has dynamic signal: {unique} unique values, peak {peak}")
    else:
        print(f"  {WARN}  Audio low-variation: {unique} unique values, peak {peak}")
    return True


print()
print("=" * 60)
print("Part 5 -- Capture verification for all configurations")
print("=" * 60)

if card_num is None:
    print(f"  {FAIL}  Card number unknown -- skipping capture tests")
    failures += 1
else:
    test_cases = [
        # (adc, rate, format, bytes_per_frame)
        ("PCM1808", 48000, "S24_3LE", 6),
        ("PCM1808", 96000, "S24_3LE", 6),
        ("CJC5430", 48000, "S24_3LE", 6),
        ("CJC5430", 96000, "S24_3LE", 6),
        ("CJC5430", 192000, "S16_LE", 4),
    ]

    for adc, rate, fmt, frame_sz in test_cases:
        print(f"\n-- Testing ADC: {adc} @ {rate} Hz ({fmt}) --")
        
        # Configure board via serial
        p = serial.Serial(port_device, 115200, timeout=1)
        p.write(f"ADC,{adc}\n".encode())
        p.readline() # OK
        p.write(f"RATE,{rate}\n".encode())
        p.readline() # OK
        p.close()
        
        # Give hardware time to settle
        time.sleep(0.2)

        wav_path = os.path.join(tempfile.gettempdir(), f"test_multi_{adc}_{rate}.wav")
        arecord_cmd = [
            "arecord",
            "-D", f"hw:{card_num},0",
            "-f", fmt,
            "-r", str(rate),
            "-c", "2",
            "-d", "1",
            wav_path,
        ]
        print(f"  Running: {' '.join(arecord_cmd)}")
        cap = subprocess.run(arecord_cmd, capture_output=True, text=True)

        if cap.returncode == 0:
            print(f"  {PASS}  arecord completed successfully")
        else:
            print(f"  {FAIL}  arecord failed with code {cap.returncode}")
            print(f"         stderr: {cap.stderr.strip()!r}")
            failures += 1
            continue

        # Check file
        min_bytes = (rate * frame_sz) - 2000
        check_audio(wav_path, min_bytes, frame_sz // 2)

print()
print("=" * 60)
if failures == 0:
    print("  ALL CONFORMANCE TESTS PASSED")
else:
    print(f"  {failures} test(s) FAILED.")
print("=" * 60)

if failures > 0:
    sys.exit(1)
