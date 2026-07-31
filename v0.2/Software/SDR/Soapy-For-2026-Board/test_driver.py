#!/usr/bin/env python3
"""
test_driver.py — End-to-end test for the Soapy2026SDR driver.

Usage:
    python3 test_driver.py [--freq 7100000] [--rate 48000]

The board must be plugged in and the driver installed
(SoapySDRUtil --find="driver=2026sdr" should show it).
"""
import sys
import argparse
import numpy as np

try:
    import SoapySDR
    from SoapySDR import SOAPY_SDR_RX, SOAPY_SDR_CF32
except ImportError as e:
    print(f"ERROR: SoapySDR Python bindings not found ({e}).")
    print("  On Ubuntu/Debian: sudo apt install python3-soapysdr")
    print("  NOTE: run this script from the project root, not from build/")
    print("        cd .. && python3 test_driver.py")
    sys.exit(1)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--freq", type=float, default=7_100_000,
                        help="Receive frequency in Hz (default: 7.1 MHz)")
    parser.add_argument("--rate", type=float, default=48000,
                        help="Sample rate in Hz (48000 or 96000)")
    args = parser.parse_args()

    # ── 1. Enumerate ─────────────────────────────────────────────────────────
    print("Searching for WWU 2026 SDR board...")
    devs = SoapySDR.Device.enumerate({"driver": "2026sdr"})
    if not devs:
        print("ERROR: No 2026 SDR board found.")
        print("  Check that the board is plugged in and the driver is installed.")
        sys.exit(1)

    print(f"  Found: {devs[0]['label']}")
    if len(devs) > 1:
        print(f"  (Using first of {len(devs)} boards found)")

    # ── 2. Open device ────────────────────────────────────────────────────────
    print("Opening device...")
    sdr = SoapySDR.Device(devs[0])
    print(f"  Hardware : {sdr.getHardwareKey()}")
    print(f"  Firmware : {sdr.readSetting('firmware_version')}")
    print(f"  Crystal  : {sdr.readSetting('crystal_freq_hz')} Hz")
    print(f"  Mode     : {sdr.readSetting('mixer_mode')}")

    # ── 3. Configure ──────────────────────────────────────────────────────────
    print(f"\nConfiguring: {args.rate/1e3:.0f} kHz sample rate, "
          f"{args.freq/1e6:.4f} MHz receive frequency")
    sdr.setSampleRate(SOAPY_SDR_RX, 0, args.rate)
    sdr.setFrequency(SOAPY_SDR_RX, 0, args.freq)

    actual_lo = sdr.getFrequency(SOAPY_SDR_RX, 0, "RF")
    actual_sr = sdr.getSampleRate(SOAPY_SDR_RX, 0)
    print(f"  Actual LO     : {actual_lo/1e6:.6f} MHz")
    print(f"  Actual rate   : {actual_sr:.0f} Hz")
    print(f"  PLL status    : {sdr.readSetting('pll_status')}")

    # ── 4. Stream ─────────────────────────────────────────────────────────────
    n_samples = int(args.rate * 0.2)  # 0.2 seconds
    print(f"\nCapturing {n_samples} samples ({0.2:.1f} s)...")

    stream = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32)
    sdr.activateStream(stream)

    buf = np.zeros(n_samples, dtype=np.complex64)
    total = 0
    CHUNK = 4096
    while total < n_samples:
        chunk = min(CHUNK, n_samples - total)
        sr = sdr.readStream(stream, [buf[total:]], chunk, timeoutUs=2_000_000)
        if sr.ret < 0:
            print(f"ERROR: readStream returned {sr.ret}")
            break
        if sr.flags & SoapySDR.SOAPY_SDR_OVERFLOW:
            print("  WARNING: overflow flag set")
        total += sr.ret

    sdr.deactivateStream(stream)
    sdr.closeStream(stream)

    # ── 5. Results ────────────────────────────────────────────────────────────
    samples = buf[:total]
    power_dBFS = 10 * np.log10(np.mean(np.abs(samples)**2) + 1e-12)
    peak_dBFS  = 20 * np.log10(np.max(np.abs(samples)) + 1e-12)

    print(f"\nResults ({total} samples captured):")
    print(f"  Mean power : {power_dBFS:.1f} dBFS")
    print(f"  Peak level : {peak_dBFS:.1f} dBFS")
    print(f"  I DC offset: {np.mean(samples.real):.6f}")
    print(f"  Q DC offset: {np.mean(samples.imag):.6f}")

    if total >= n_samples * 0.9:
        print("\nPASS ✓ — driver is working correctly.")
    else:
        print(f"\nFAIL ✗ — only captured {total}/{n_samples} samples.")
        sys.exit(1)

if __name__ == "__main__":
    main()
