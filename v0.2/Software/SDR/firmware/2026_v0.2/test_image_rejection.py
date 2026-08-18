#!/usr/bin/env python3
import sys
import subprocess
import argparse
import numpy as np

def capture_audio(device="hw:2,0", rate=96000, duration=5):
    """Captures raw audio directly from arecord via a pipe."""
    cmd = [
        "arecord",
        "-D", device,
        "-c", "2",
        "-r", str(rate),
        "-f", "S24_3LE",
        "-d", str(duration),
        "-t", "raw",
        "-q"
    ]
    print(f"[*] Capturing {duration}s of live RF from {device} at {rate} Hz...")
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    raw_data, err = proc.communicate()

    if proc.returncode != 0:
        print(f"[!] arecord error:\n{err.decode('utf-8')}")
        sys.exit(1)

    # Unpack 24-bit (3-byte) PCM to 32-bit float
    a = np.frombuffer(raw_data, dtype=np.uint8).reshape(-1, 3)
    a = np.pad(a, ((0, 0), (0, 1)), mode='constant')
    samples = (a.view(np.int32).reshape(-1, 2) >> 8).astype(np.float64)

    return samples[:, 0], samples[:, 1]

def analyze_live_rf(I, Q, rate, dc_notch_hz=500):
    N = len(I)

    # 1. Remove DC Offset
    I = I - np.mean(I)
    Q = Q - np.mean(Q)

    # 2. Broadband Statistical Estimation
    rms_i = np.sqrt(np.mean(I**2))
    rms_q = np.sqrt(np.mean(Q**2))
    
    gain_ratio = rms_i / (rms_q + 1e-12)
    amp_imb_db = 20 * np.log10(gain_ratio)

    cross_corr = np.mean(I * Q) / (rms_i * rms_q + 1e-12)
    cross_corr = np.clip(cross_corr, -1.0, 1.0)
    phase_err_deg = np.degrees(np.arcsin(cross_corr))
    phase_rad = np.radians(phase_err_deg)

    # Statistical IRR formula
    irr_num = 1.0 + 2.0 * gain_ratio * np.cos(phase_rad) + gain_ratio**2
    irr_den = 1.0 - 2.0 * gain_ratio * np.cos(phase_rad) + gain_ratio**2
    stat_irr = 10 * np.log10(max(irr_num / (irr_den + 1e-12), 1.0))

    # Theoretical statistical measurement ceiling: IRR_max ~ 10*log10(4 * N)
    stat_irr_limit = 10 * np.log10(4.0 * N)

    # 3. Spectral Welch Averaging
    nperseg = 8192
    step = nperseg // 2
    n_chunks = (N - nperseg) // step

    window = np.blackman(nperseg)
    psd_accum = np.zeros(nperseg)

    z = I + 1j * Q
    for i in range(n_chunks):
        chunk = z[i * step : i * step + nperseg] * window
        fft_chunk = np.fft.fftshift(np.fft.fft(chunk))
        psd_accum += np.abs(fft_chunk)**2

    psd_avg = psd_accum / n_chunks
    psd_db = 10 * np.log10(psd_avg + 1e-12)
    freqs = np.fft.fftshift(np.fft.fftfreq(nperseg, 1.0 / rate))

    # 4. Off-Air Peak Hunting
    valid_mask = np.abs(freqs) > dc_notch_hz
    valid_indices = np.where(valid_mask)[0]
    noise_floor_db = np.median(psd_db[valid_mask])
    
    peaks = []
    for idx in valid_indices[1:-1]:
        if psd_db[idx] > psd_db[idx - 1] and psd_db[idx] > psd_db[idx + 1]:
            if psd_db[idx] > noise_floor_db + 6.0:  # Minimum 6 dB SNR to evaluate
                peaks.append((idx, freqs[idx], psd_db[idx]))

    peaks.sort(key=lambda x: x[2], reverse=True)

    # ------------------ REPORT OUTPUT ------------------
    print("\n" + "=" * 68)
    print("           QSD SDR IMAGE REJECTION & DYNAMIC RANGE REPORT           ")
    print("=" * 68)
    print(f"Capture Statistics     : {N:,} samples ({N/rate:.1f} sec @ {rate} Hz)")
    print(f"I Channel Noise RMS    : {rms_i:10.1f}")
    print(f"Q Channel Noise RMS    : {rms_q:10.1f}")
    print(f"Amplitude Imbalance    : {amp_imb_db:+7.2f} dB")
    print(f"Quadrature Phase Error : {phase_err_deg:+7.2f}° (from 90°)")
    print("-" * 68)
    print(f"Estimated Broadband IRR: {stat_irr:7.2f} dB")
    print(f"  └─ Measurement Cap   : {stat_irr_limit:7.2f} dB  (sample-size statistical limit)")
    print("-" * 68)

    print("\n--- Detected Off-Air Signals & Rejection ---")
    if not peaks:
        print("  [i] No strong discrete signals found.")
        print(f"      (Broadband noise estimate above is accurate up to ~{stat_irr_limit:.1f} dB)")
    else:
        print(f"{'Signal Freq':<13} {'SNR':<8} {'Max Meas. Cap':<15} {'Measured IRR':<14} {'Status'}")
        print("-" * 68)
        tested_pairs = []
        for idx, f_sig, p_sig in peaks[:6]:
            f_img_target = -f_sig
            if any(abs(abs(f_sig) - abs(f_prev)) < 200 for f_prev in tested_pairs):
                continue
            tested_pairs.append(f_sig)

            img_idx = np.argmin(np.abs(freqs - f_img_target))
            p_img = psd_db[img_idx]
            snr = p_sig - noise_floor_db
            rejection = p_sig - p_img

            # A peak cannot measure an image lower than its own SNR
            max_peak_irr_cap = snr

            if rejection >= (max_peak_irr_cap - 2.0):
                status = "★ Noise-Floor Limited (Actual IRR >= Measured)"
            else:
                status = "✓ Direct Measurement"

            print(f"{f_sig:+8.1f} Hz   {snr:5.1f}dB   {max_peak_irr_cap:5.1f} dB (SNR)    {rejection:5.1f} dB        {status}")

    print("=" * 68)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Live SDR I/Q Balance & Image Rejection Meter")
    parser.add_argument("-d", "--device", default="hw:2,0", help="ALSA device (default: hw:2,0)")
    parser.add_argument("-r", "--rate", type=int, default=96000, help="Sample rate (default: 96000)")
    parser.add_argument("-t", "--time", type=int, default=5, help="Capture time in seconds (default: 5)")
    args = parser.parse_args()

    I, Q = capture_audio(device=args.device, rate=args.rate, duration=args.time)
    analyze_live_rf(I, Q, rate=args.rate)
