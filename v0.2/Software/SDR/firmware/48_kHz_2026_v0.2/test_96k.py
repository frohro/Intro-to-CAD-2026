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
# Part 1 -- Find CDC port  (VID:PID 0xcafe:0x4011)
# ---------------------------------------------------------------------------
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
check("line 1 starts with VER,", r[0][:4], "VER,")
check("line 2 = OK",              r[1],     "OK")

# XTAL
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

# RATE 96000 -- research firmware stores this as the pending rate
print("\n-- RATE,96000 --")
r = cmd("RATE,96000", n=1)
check("line 1 = OK", r[0], "OK")

# RATE 48000 -- firmware must accept both rates
print("\n-- RATE,48000 --")
r = cmd("RATE,48000", n=1)
check("line 1 = OK", r[0], "OK")

# Restore 96000 for the audio tests
cmd("RATE,96000", n=1)

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
# lsusb shows one bSamFreqType line per alt setting, so expect at least two
samfreq_matches = re.findall(r"bSamFreqType\s+(\d+)", lsusb_out)
n_samfreq1 = samfreq_matches.count("1")
if n_samfreq1 >= 2:
    print(f"  {PASS}  bSamFreqType = 1 on both alt settings ({n_samfreq1} found)")
else:
    print(f"  {FAIL}  Expected bSamFreqType=1 on two alt settings, got: {samfreq_matches}")
    failures += 1

# Check both frequencies present
has_48k = bool(re.search(r"48000", lsusb_out))
has_96k = bool(re.search(r"96000", lsusb_out))
check("48000 Hz advertised in descriptor", has_48k, True)
check("96000 Hz advertised in descriptor", has_96k, True)

# Check wMaxPacketSize: 294 B on alt 1 (48 kHz), 582 B on alt 2 (96 kHz)
# These specific byte counts also confirm the two-alt-setting design is
# actually flashed (not the old single-alt 776 B design).
has_294 = bool(re.search(r"\b294\b", lsusb_out))
has_582 = bool(re.search(r"\b582\b", lsusb_out))
check("alt 1 wMaxPacketSize = 294 B (48 kHz, Single-TT safe)", has_294, True)
check("alt 2 wMaxPacketSize = 582 B (96 kHz)", has_582, True)

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
# Part 5a -- Audio capture at 96 kHz
# ---------------------------------------------------------------------------
print()
print("=" * 60)
print("Part 5a -- Audio capture at 96 kHz S24_3LE (1 second)")
print("=" * 60)

if card_num is None:
    print(f"  {FAIL}  Card number unknown -- skipping capture tests")
    failures += 1
else:
    # With the two-alt-setting design the sample rate is determined by
    # which alt snd-usb-audio selects; arecord -r 96000 selects alt 2.
    print(f"  Using ALSA card {card_num}...")
    wav_96k = os.path.join(tempfile.gettempdir(), "test_96k_96000.wav")
    arecord_cmd = [
        "arecord",
        "-D", f"hw:{card_num},0",
        "-f", "S24_3LE",
        "-r", "96000",
        "-c", "2",
        "-d", "1",
        wav_96k,
    ]
    print(f"  Running: {' '.join(arecord_cmd)}")
    cap = subprocess.run(arecord_cmd, capture_output=True, text=True)

    if cap.returncode == 0:
        print(f"  {PASS}  arecord exited 0")
    else:
        print(f"  {FAIL}  arecord exited {cap.returncode}")
        print(f"         stderr: {cap.stderr.strip()!r}")
        failures += 1

    MIN_96K = 400_000  # 1 s at 96 kHz stereo S24_3LE = 576000 bytes + 44 header
    check_audio("96 kHz audio content", wav_96k, MIN_96K, 5760)


# ---------------------------------------------------------------------------
# Part 5b -- Audio capture at 48 kHz (backward compatibility)
# ---------------------------------------------------------------------------
print()
print("=" * 60)
print("Part 5b -- Audio capture at 48 kHz S24_3LE (backward compat)")
print("=" * 60)

if card_num is None:
    print(f"  {FAIL}  Card number unknown -- skipping")
    failures += 1
else:
    # arecord -r 48000 causes snd-usb-audio to select alt 1 (294 B packets).
    wav_48k = os.path.join(tempfile.gettempdir(), "test_96k_48000.wav")
    arecord_cmd = [
        "arecord",
        "-D", f"hw:{card_num},0",
        "-f", "S24_3LE",
        "-r", "48000",
        "-c", "2",
        "-d", "1",
        wav_48k,
    ]
    print(f"  Running: {' '.join(arecord_cmd)}")
    cap = subprocess.run(arecord_cmd, capture_output=True, text=True)

    if cap.returncode == 0:
        print(f"  {PASS}  arecord at 48 kHz exited 0")
    else:
        print(f"  {FAIL}  arecord at 48 kHz exited {cap.returncode}")
        print(f"         stderr: {cap.stderr.strip()!r}")
        failures += 1

    MIN_48K = 200_000  # 1 s at 48 kHz stereo S24_3LE = 288000 bytes + 44 header
    check_audio("48 kHz audio content", wav_48k, MIN_48K, 2880)


# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
print()
print("=" * 60)
if failures == 0:
    print(f"\033[32mAll tests passed.\033[0m")
    print("Research 96 kHz firmware meets protocol and audio requirements.")
else:
    print(f"\033[31m{failures} test(s) FAILED.\033[0m")
    sys.exit(1)
