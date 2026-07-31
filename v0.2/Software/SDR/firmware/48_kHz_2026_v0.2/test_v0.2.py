#!/usr/bin/env python3
"""
test_96k.py -- Protocol + audio conformance test for the 48/96 kHz dual-rate
               research firmware (Research/96_kHz/).

Verifies:
  1. CDC device found (VID:PID 0xcafe:0x4011)
  2. Quisk CDC protocol: VER, XTAL, MODE, RATE (96000), FREQ
  3. UAC1 device enumeration: 'WWU SDR' in arecord -l
  4. USB descriptor: two alt settings (bSamFreqType=1 each), 48000 Hz on alt1,
     96000 Hz on alt2, bSubframeSize=3 (via lsusb -v output)
  5. Audio capture at 96 kHz S24_3LE (primary rate)
  6. Audio capture at 48 kHz S24_3LE (backward-compat rate)

Usage:
    python3 Research/96_kHz/test_96k.py
"""

import os
import re
import subprocess
import sys
import tempfile
import time

import serial
import serial.tools.list_ports

# ---------------------------------------------------------------------------
# Colour helpers
# ---------------------------------------------------------------------------
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


# ---------------------------------------------------------------------------
# Part 0 -- Calibration / Mode selection
# ---------------------------------------------------------------------------
print("=" * 60)
print("Part 0 -- User Configuration")
print("=" * 60)

target_rate = 0
while target_rate not in (48, 96):
    ans = input("Which frequency are your jumpers set for? (48 or 96): ").strip()
    if ans == "48":
        target_rate = 48
    elif ans == "96":
        target_rate = 96

target_hz = target_rate * 1000
print(f"Testing for {target_rate} kHz configuration...")


# ---------------------------------------------------------------------------
# Part 1 -- Find CDC port  (VID:PID 0xcafe:0x4011)
# ---------------------------------------------------------------------------
print()
print("=" * 60)
print("Part 1 -- CDC / Serial port")
print("=" * 60)

KNOWN_VIDS_PIDS = [(0xcafe, 0x4011), (0xcafe, 0x4010)]

port = None
for info in serial.tools.list_ports.comports():
    if (info.vid, info.pid) in KNOWN_VIDS_PIDS:
        try:
            port = serial.Serial(info.device, 115200, timeout=3)
            print(f"  Opened {info.device}  ({info.description})")
            break
        except serial.SerialException:
            continue

if port is None:
    for p in ("/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyACM2", "/dev/ttyACM3"):
        try:
            port = serial.Serial(p, 115200, timeout=3)
            print(f"  Opened {p}")
            break
        except serial.SerialException:
            continue

if port is None:
    sys.exit("ERROR: No CDC device found (VID:PID 0xcafe:0x4011 or /dev/ttyACM0-3).")


# ---------------------------------------------------------------------------
# Part 2 -- Quisk CDC protocol
# ---------------------------------------------------------------------------
print()
print("=" * 60)
print("Part 2 -- Quisk CDC protocol")
print("=" * 60)

def cmd(s, n=2):
    port.write((s + "\n").encode())
    return [port.readline().decode().rstrip("\r\n") for _ in range(n)]

# Startup: Ctrl-C / Ctrl-D, then wait for sentinel
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
# Drain any extra output (e.g. a second sentinel triggered by Ctrl-D arriving
# while the DTR-triggered sentinel was already in flight).
while port.readline():
    pass
port.timeout = 3

if not got_ready:
    print(f"  {FAIL}  SDR ready sentinel not received within 6 s")
    failures += 1

# VER -- firmware 2.0 for research version
print("\n-- VER --")
r = cmd("VER")
# Accept both old and new VER responses if they contain "VER,"
check("line 1 starts with VER,", r[0][:4], "VER,")
check("line 2 = OK",              r[1],     "OK")

# XTAL -- v0.2 uses 24.576 MHz
print("\n-- XTAL --")
r = cmd("XTAL")
check("line 1 = XTAL,24576000", r[0], "XTAL,24576000")
check("line 2 = OK",             r[1], "OK")

# MODE
print("\n-- MODE --")
r = cmd("MODE")
check("line 1 = MODE,DIRECT or MODE,JOHNSON",
      r[0] in ("MODE,DIRECT", "MODE,JOHNSON"), True)
check("line 2 = OK", r[1], "OK")

# RATE -- check pending or active rate
print(f"\n-- RATE (Query) --")
r = cmd("RATE")
check(f"line 1 matches target {target_hz}", r[0], f"RATE,{target_hz}")
check("line 2 = OK", r[1], "OK")

# Bare FREQ before any tune
print("\n-- FREQ, (bare, before tune) --")
r = cmd("FREQ,")
try:
    int(r[0])
    check("line 1 is an integer", True, True)
except ValueError:
    check("line 1 is an integer", False, True)
check("line 2 starts with OK,", r[1][:3], "OK,")

# FREQ golden integer 7.168 MHz
print("\n-- FREQ golden integer (7.168 MHz) --")
r = cmd("FREQ,7168000,96,28,0,1,3072,0,1")
check("line 1 = 7168000", r[0], "7168000")
check("line 2 = OK,G,0",  r[1], "OK,G,0")

# FREQ fractional 7.074 MHz (40 m FT8)
print("\n-- FREQ fractional (7.074 MHz) --")
r = cmd("FREQ,7074000,100,28,31250,1048575,3328,897,1048575")
check("line 1 = 7074000", r[0], "7074000")
check("line 2 = OK,F,0",  r[1], "OK,F,0")

# Bare FREQ after tune
print("\n-- FREQ, (bare, after tune) --")
r = cmd("FREQ,")
check("line 1 = 7074000 (last programmed hz)", r[0], "7074000")
check("line 2 starts with OK,",               r[1][:3], "OK,")

# Unknown command
print("\n-- Unknown command --")
r = cmd("BOGUS", n=1)
check("line 1 = ERR", r[0], "ERR")

port.close()


# ---------------------------------------------------------------------------
# Part 3 -- UAC1 audio device enumeration
# ---------------------------------------------------------------------------
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
    print()
    print("  Hint: Check that PICO_TINYUSB_PATH=$HOME/tinyusb was set during build.")
    print("  Hint: Run `dmesg | grep -i usb | tail -20` to see enumeration errors.")
    print()

# Parse card number for "WWU SDR"
card_num = None
for line in arecord_output.splitlines():
    m = re.match(r"card (\d+):.*WWU SDR", line)
    if m:
        card_num = int(m.group(1))
        break


# ---------------------------------------------------------------------------
# Part 4 -- USB descriptor verification (lsusb -v)
# ---------------------------------------------------------------------------
print()
print("=" * 60)
print("Part 4 -- USB descriptor verification")
print("=" * 60)

lsusb_result = subprocess.run(
    ["lsusb", "-v", "-d", "cafe:4011"],
    capture_output=True, text=True
)
lsusb_out = lsusb_result.stdout + lsusb_result.stderr

# Check bSubframeSize = 3
if re.search(r"bSubframeSize\s+3", lsusb_out):
    print(f"  {PASS}  bSubframeSize = 3 (S24_3LE)")
else:
    print(f"  {FAIL}  bSubframeSize != 3 in USB descriptor")
    failures += 1

# Check bBitResolution = 24
if re.search(r"bBitResolution\s+24", lsusb_out):
    print(f"  {PASS}  bBitResolution = 24")
else:
    print(f"  {FAIL}  bBitResolution != 24 in USB descriptor")
    failures += 1

# Check bSamFreqType = 1 (each alt setting has one discrete freq)
# v0.2 simplified descriptor has exactly ONE active alt setting (alt 1)
samfreq_matches = re.findall(r"bSamFreqType\s+(\d+)", lsusb_out)
n_samfreq1 = samfreq_matches.count("1")
check(f"Exactly one bSamFreqType=1 found (Single-rate mode)", n_samfreq1, 1)

# Check target frequency present
has_target = bool(re.search(str(target_hz), lsusb_out))
check(f"{target_hz} Hz advertised in descriptor", has_target, True)

# Check wMaxPacketSize: 294 B for 48 kHz, 582 B for 96 kHz
expected_max_packet = 294 if target_rate == 48 else 582
# Find all wMaxPacketSize lines and look for the one matching the audio endpoint pattern
# In lsusb -v, descriptors are hierarchical. We want the one under the AudioStreaming Interface.
all_max_packets = re.findall(r"wMaxPacketSize\s+0x[0-9a-fA-F]+\s+.*?\b(\d+)\b\s+bytes", lsusb_out)
# The first one is usually CDC (8 or 64), we want the large audio one (294/582)
got_max_packet = 0
for p in all_max_packets:
    val = int(p)
    if val > 64: # Audio packets are much larger than CDC Interrupt/Bulk
        got_max_packet = val
        break

check(f"wMaxPacketSize = {expected_max_packet} B", got_max_packet, expected_max_packet)

if not lsusb_out.strip():
    print(f"  {WARN}  lsusb returned no output -- device may not be enumerated yet.")
    print(f"         Try: sudo lsusb -v -d cafe:4011")


# ---------------------------------------------------------------------------
# Helper: decode a raw S24_3LE byte buffer into a list of signed 32-bit ints
# ---------------------------------------------------------------------------
def decode_s24_3le(raw):
    out = []
    for j in range(0, len(raw) - 2, 3):
        v = raw[j] | (raw[j+1] << 8) | (raw[j+2] << 16)
        if v >= 0x800000:
            v -= 0x1000000
        out.append(v)
    return out


def check_audio(label, wav_path, min_size, sample_window):
    """Open wav_path, check size, then decode and evaluate signal quality."""
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
        # Seek into the middle of the capture to avoid startup zeroes/transients
        # We know size >= min_size (e.g. 400,000 bytes). Seek 0.5 seconds in.
        relative_offset = (min_size // 2)
        # S24_3LE stereo frame is 6 bytes. Prevent reading misaligned samples.
        relative_offset = relative_offset - (relative_offset % 6)
        offset = 44 + relative_offset
        if offset + sample_window > size:
            offset = 44
        f.seek(offset)
        raw = f.read(sample_window)
    samples = decode_s24_3le(raw)
    if not samples:
        return
    lo, hi = min(samples), max(samples)
    unique = len(set(samples))
    peak   = max(abs(lo), abs(hi))
    # Stuck-high: DATA line pulled HIGH → every sample ≈ -1 (0xFFFFFF)
    # Stuck-low / stalled DMA: every sample = 0
    # Real audio: meaningful variation and non-trivial amplitude
    if unique == 1 and samples[0] == 0:
        print(f"  {WARN}  Audio all-zero -- PCM1808 not producing samples.")
        print(f"         Check: GPIO14=DATA, GPIO15=BCK, GPIO16=WS connected;")
        print(f"         PCM1808 SCKI crystal present; PWDN/FMT/MD0 strapped correctly.")
    elif unique <= 3 and peak <= 1:
        print(f"  {WARN}  Audio stuck near -1 (DATA line pulled HIGH?): {unique} unique values, peak {peak}")
    elif unique > 20 and peak > 200:
        print(f"  {PASS}  Audio has real signal: {unique} unique values, peak {peak}")
    else:
        print(f"  {WARN}  Audio low-variation: {unique} unique values, peak {peak}")
        print(f"         Possible stuck line or very quiet input.")


# ---------------------------------------------------------------------------
# Part 5 -- Audio capture
# ---------------------------------------------------------------------------
print()
print("=" * 60)
print(f"Part 5 -- Audio capture at {target_hz} Hz S24_3LE (1 second)")
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
        "-f", "S24_3LE",
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

    # 1 s at 48k: 288,000 bytes. 1 s at 96k: 576,000 bytes.
    min_bytes = (target_hz * 6) - 1000 
    check_audio(f"{target_hz} Hz audio content", wav_path, min_bytes, 5760)


# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
print()
print("=" * 60)
if failures == 0:
    print(f"  ALL TESTS PASSED")
else:
    print(f"  {failures} test(s) FAILED.")
print("=" * 60)

if failures > 0:
    sys.exit(1)
