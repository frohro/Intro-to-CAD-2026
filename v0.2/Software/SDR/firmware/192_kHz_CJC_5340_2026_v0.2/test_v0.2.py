#!/usr/bin/env python3
"""
test_v0.2.py -- Protocol + audio conformance test for fixed-rate 192 kHz
               research firmware (Research/192_kHz_CJC_5340_2026_v0.2/).

Verifies:
  1. CDC device found (VID:PID 0xcafe:0x4011)
  2. Quisk CDC protocol: VER, XTAL, MODE, RATE (192000), FREQ
  3. UAC1 device enumeration: 'WWU SDR' in arecord -l
  4. USB descriptor: bSamFreqType=1, 192000 Hz, bSubframeSize=2,
     bBitResolution=16, wMaxPacketSize=772
  5. Audio capture at 192 kHz S16_LE

Usage:
    python3 Research/192_kHz_CJC_5340_2026_v0.2/test_v0.2.py
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
print("Part 0 -- User Configuration")
print("=" * 60)

target_hz = 192000
print(f"Testing for fixed {target_hz} Hz configuration...")


print()
print("=" * 60)
print("Part 1 -- CDC / Serial port")
print("=" * 60)

KNOWN_VIDS_PIDS = [(0xcafe, 0x4011), (0xcafe, 0x4010)]

port = None
found_vid, found_pid = None, None
for info in serial.tools.list_ports.comports():
    if (info.vid, info.pid) in KNOWN_VIDS_PIDS:
        try:
            port = serial.Serial(info.device, 115200, timeout=3)
            print(f"  Opened {info.device}  ({info.description})")
            found_vid, found_pid = info.vid, info.pid
            break
        except serial.SerialException:
            continue

if port is None:
    for p in ("/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyACM2", "/dev/ttyACM3"):
        try:
            port = serial.Serial(p, 115200, timeout=3)
            print(f"  Opened {p}")
            found_vid, found_pid = 0xcafe, 0x4011
            break
        except serial.SerialException:
            continue

if port is None:
    sys.exit("ERROR: No CDC device found (VID:PID 0xcafe:0x4011 or /dev/ttyACM0-3).")


print()
print("=" * 60)
print("Part 2 -- Quisk CDC protocol")
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
check("line 1 = MODE,DIRECT or MODE,JOHNSON",
      r[0] in ("MODE,DIRECT", "MODE,JOHNSON"), True)
check("line 2 = OK", r[1], "OK")

print("\n-- RATE (Query) --")
r = cmd("RATE")
check(f"line 1 matches target {target_hz}", r[0], f"RATE,{target_hz}")
check("line 2 = OK", r[1], "OK")

print("\n-- FREQ, (bare, before tune) --")
r = cmd("FREQ,")
try:
    int(r[0])
    check("line 1 is an integer", True, True)
except ValueError:
    check("line 1 is an integer", False, True)
check("line 2 starts with OK,", r[1][:3], "OK,")

print("\n-- FREQ golden integer (7.168 MHz) --")
r = cmd("FREQ,7168000,96,28,0,1,3072,0,1")
check("line 1 = 7168000", r[0], "7168000")
check("line 2 = OK,G,0",  r[1], "OK,G,0")

print("\n-- FREQ fractional (7.074 MHz) --")
r = cmd("FREQ,7074000,100,28,31250,1048575,3328,897,1048575")
check("line 1 = 7074000", r[0], "7074000")
check("line 2 = OK,F,0",  r[1], "OK,F,0")

print("\n-- FREQ, (bare, after tune) --")
r = cmd("FREQ,")
check("line 1 = 7074000 (last programmed hz)", r[0], "7074000")
check("line 2 starts with OK,",               r[1][:3], "OK,")

print("\n-- Unknown command --")
r = cmd("BOGUS", n=1)
check("line 1 = ERR", r[0], "ERR")

port.close()


print()
print("=" * 60)
print("Part 3 -- UAC1 audio device enumeration")
print("=" * 60)

result = subprocess.run(["arecord", "-l"], capture_output=True, text=True)
arecord_output = result.stdout + result.stderr

print()
print("  arecord -l output:")
for line in arecord_output.splitlines():
    print(f"    {line}")
print()

if "WWU SDR" in arecord_output:
    print(f"  {PASS}  'WWU SDR' found in arecord -l output")
else:
    print(f"  {FAIL}  'WWU SDR' NOT found in arecord -l output")
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

device_id = f"{found_vid:04x}:{found_pid:04x}" if found_vid else "cafe:4011"
lsusb_result = subprocess.run(
    ["lsusb", "-v", "-d", device_id],
    capture_output=True, text=True
)
lsusb_out = lsusb_result.stdout + lsusb_result.stderr

if re.search(r"bSubframeSize\s+2", lsusb_out):
    print(f"  {PASS}  bSubframeSize = 2 (S16_LE)")
else:
    print(f"  {FAIL}  bSubframeSize != 2 in USB descriptor")
    failures += 1

if re.search(r"bBitResolution\s+16", lsusb_out):
    print(f"  {PASS}  bBitResolution = 16")
else:
    print(f"  {FAIL}  bBitResolution != 16 in USB descriptor")
    failures += 1

samfreq_matches = re.findall(r"bSamFreqType\s+(\d+)", lsusb_out)
n_samfreq1 = samfreq_matches.count("1")
check("Exactly one bSamFreqType=1 found (Fixed-rate mode)", n_samfreq1, 1)

has_target = bool(re.search(str(target_hz), lsusb_out))
check(f"{target_hz} Hz advertised in descriptor", has_target, True)

expected_max_packet = 772
all_max_packets = re.findall(r"wMaxPacketSize\s+0x[0-9a-fA-F]+\s+.*?\b(\d+)\b\s+bytes", lsusb_out)
got_max_packet = 0
for p in all_max_packets:
    val = int(p)
    if val > 64:
        got_max_packet = val
        break

check(f"wMaxPacketSize = {expected_max_packet} B", got_max_packet, expected_max_packet)

if not lsusb_out.strip():
    print(f"  {WARN}  lsusb returned no output -- device may not be enumerated yet.")


def decode_s16_le(raw):
    out = []
    for j in range(0, len(raw) - 1, 2):
        v = raw[j] | (raw[j + 1] << 8)
        if v >= 0x8000:
            v -= 0x10000
        out.append(v)
    return out


def check_audio(wav_path, min_size, sample_window):
    global failures
    if not os.path.exists(wav_path):
        print(f"  {FAIL}  WAV file not created at {wav_path}")
        failures += 1
        return
    size = os.path.getsize(wav_path)
    if size >= min_size:
        print(f"  {PASS}  WAV file size {size} bytes (>= {min_size})")
    else:
        print(f"  {FAIL}  WAV file size {size} bytes (< {min_size})")
        failures += 1

    with open(wav_path, "rb") as f:
        relative_offset = (min_size // 2)
        relative_offset = relative_offset - (relative_offset % 4)
        offset = 44 + relative_offset
        if offset + sample_window > size:
            offset = 44
        f.seek(offset)
        raw = f.read(sample_window)

    samples = decode_s16_le(raw)
    if not samples:
        return

    lo, hi = min(samples), max(samples)
    unique = len(set(samples))
    peak = max(abs(lo), abs(hi))

    if unique == 1 and samples[0] == 0:
        print(f"  {WARN}  Audio all-zero -- ADC not producing samples.")
    elif unique > 20 and peak > 20:
        print(f"  {PASS}  Audio has real signal: {unique} unique values, peak {peak}")
    else:
        print(f"  {WARN}  Audio low-variation: {unique} unique values, peak {peak}")


print()
print("=" * 60)
print(f"Part 5 -- Audio capture at {target_hz} Hz S16_LE (1 second)")
print("=" * 60)

if card_num is None:
    print(f"  {FAIL}  Card number unknown -- skipping capture tests")
    failures += 1
else:
    print(f"  Using ALSA card {card_num}...")
    wav_path = os.path.join(tempfile.gettempdir(), f"test_v0.2_{target_hz}.wav")
    arecord_cmd = [
        "arecord",
        "-D", f"hw:{card_num},0",
        "-f", "S16_LE",
        "-r", str(target_hz),
        "-c", "2",
        "-d", "1",
        wav_path,
    ]
    print(f"  Running: {' '.join(arecord_cmd)}")
    cap = subprocess.run(arecord_cmd, capture_output=True, text=True)

    if cap.returncode == 0:
        print(f"  {PASS}  arecord exited 0")
    else:
        print(f"  {FAIL}  arecord exited {cap.returncode}")
        print(f"         stderr: {cap.stderr.strip()!r}")
        failures += 1

    min_bytes = (target_hz * 4) - 1000
    check_audio(wav_path, min_bytes, 8192)


print()
print("=" * 60)
if failures == 0:
    print("  ALL TESTS PASSED")
else:
    print(f"  {failures} test(s) FAILED.")
print("=" * 60)

if failures > 0:
    sys.exit(1)
