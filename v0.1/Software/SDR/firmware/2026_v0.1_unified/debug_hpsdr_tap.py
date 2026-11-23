#!/usr/bin/env python3
"""
Wi-Fi / UDP Tap and Live Packet Inspector for OpenHPSDR Protocol 1
------------------------------------------------------------------
This script intercepts or sniffs UDP traffic between Quisk and the Pico W
to diagnose exactly which step in the handshake stalls or fails.

Usage:
  # Mode 1: Proxy/Bridge Mode (Point Quisk to localhost:1024, Bridge forwards to Pico W:1024)
  python3 debug_hpsdr_tap.py --proxy --pico-ip 192.168.1.186

  # Mode 2: Sniffer Mode (Passively listens to broadcasts and verifies packet stream)
  python3 debug_hpsdr_tap.py --sniff
"""

import sys
import socket
import struct
import time
import argparse
import select
import math

def parse_args():
    parser = argparse.ArgumentParser(description="OpenHPSDR Wi-Fi Bridge & Diagnostic Protocol Tap with Live IQ Zero Checker")
    parser.add_argument("--proxy", action="store_true", default=True, help="Run as transparent proxy on 0.0.0.0:1025 or 1024")
    parser.add_argument("--pico-ip", type=str, default="192.168.1.186", help="Pico W actual IP address")
    parser.add_argument("--local-port", type=int, default=1025, help="Port to listen for Quisk (e.g. 1025)")
    parser.add_argument("--pico-port", type=int, default=1024, help="Port where Pico W listens (1024)")
    return parser.parse_args()

def unpack_s24_be(b0, b1, b2):
    """Converts 3 big-endian bytes to signed 24-bit integer."""
    val = (b0 << 16) | (b1 << 8) | b2
    if val & 0x800000:
        val -= 0x1000000
    return val

def analyze_iq_payload(data):
    """Parses 126 stereo 24-bit IQ samples from an OpenHPSDR EP6 1032-byte frame."""
    if len(data) != 1032:
        return None

    samples_i = []
    samples_q = []
    zero_count = 0

    # Subframe 1: 63 samples starting at offset 16 (stride 8 bytes)
    for s in range(63):
        off = 16 + s * 8
        i_val = unpack_s24_be(data[off], data[off+1], data[off+2])
        q_val = unpack_s24_be(data[off+3], data[off+4], data[off+5])
        if i_val == 0 and q_val == 0:
            zero_count += 1
        samples_i.append(i_val)
        samples_q.append(q_val)

    # Subframe 2: 63 samples starting at offset 528 (stride 8 bytes)
    for s in range(63):
        off = 528 + s * 8
        i_val = unpack_s24_be(data[off], data[off+1], data[off+2])
        q_val = unpack_s24_be(data[off+3], data[off+4], data[off+5])
        if i_val == 0 and q_val == 0:
            zero_count += 1
        samples_i.append(i_val)
        samples_q.append(q_val)

    total_samples = 126
    is_all_zero = (zero_count == total_samples)

    peak_i = max(abs(x) for x in samples_i)
    peak_q = max(abs(x) for x in samples_q)
    peak = max(peak_i, peak_q)

    # Calculate RMS in dBFS (24-bit full scale = 8388607)
    sum_sq = sum((i / 8388608.0)**2 + (q / 8388608.0)**2 for i, q in zip(samples_i, samples_q))
    mean_sq = sum_sq / (2.0 * total_samples) if total_samples > 0 else 0.0
    rms_dbfs = 10.0 * math.log10(mean_sq) if mean_sq > 1e-12 else -180.0

    return {
        "is_all_zero": is_all_zero,
        "zero_count": zero_count,
        "total_samples": total_samples,
        "peak": peak,
        "peak_i": peak_i,
        "peak_q": peak_q,
        "rms_dbfs": rms_dbfs,
        "first_i": samples_i[:3],
        "first_q": samples_q[:3],
        "first_raw_hex": data[16:32].hex()
    }

def run_proxy(pico_ip, pico_port, local_port):
    print("=" * 70)
    print(" OPENHPSDR PROTOCOL 1 TRANSPARENT DIAGNOSTIC PROXY")
    print("=" * 70)
    print(f"[*] Binding Local Proxy Socket on 0.0.0.0:{local_port}")
    print(f"[*] Target Pico W: {pico_ip}:{pico_port}")
    print("\n--> IN QUISK CONFIG (or quisk_conf_openhpsdr.py):")
    print(f"    Set hermes_ip = '127.0.0.1'")
    print(f"    Set hermes_port = {local_port}")
    print("=" * 70)
    print("[*] Waiting for Quisk / SDR software to connect...\n")

    # Socket facing Quisk (Client)
    sock_quisk = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock_quisk.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock_quisk.bind(("0.0.0.0", local_port))

    # Socket facing Pico W (Server)
    sock_pico = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock_pico.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    quisk_addr = None
    pico_target = (pico_ip, pico_port)

    quisk_pkts = 0
    pico_pkts = 0
    start_time = time.time()

    ep6_audio_count = 0
    ep6_zero_frames = 0
    ep6_nonzero_frames = 0
    ep2_cc_count = 0
    discovery_count = 0
    start_cmd_count = 0
    stop_cmd_count = 0
    last_zero_status = None

    try:
        while True:
            r, _, _ = select.select([sock_quisk, sock_pico], [], [], 0.5)
            now = time.time()

            for s in r:
                if s is sock_pico:
                    # Packet received from Pico W -> Forward to Quisk
                    data, addr = sock_pico.recvfrom(2048)
                    pico_pkts += 1

                    if len(data) == 60 and data[0] == 0xEF and data[1] == 0xFE:
                        board_id = data[10]
                        mac = ":".join(f"{b:02X}" for b in data[3:9])
                        print(f"[{now - start_time:7.2f}s] [PICO -> QUISK] Discovery Reply (60B) | Board: 0x{board_id:02X}, MAC: {mac}")
                    elif len(data) == 1032 and data[0] == 0xEF and data[1] == 0xFE:
                        ep6_audio_count += 1
                        seq = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7]
                        c0_1 = data[11]
                        c0_2 = data[523] if len(data) > 523 else 0

                        # Perform deep I/Q sample inspection
                        iq_stats = analyze_iq_payload(data)
                        if iq_stats:
                            if iq_stats["is_all_zero"]:
                                ep6_zero_frames += 1
                            else:
                                ep6_nonzero_frames += 1

                            # Print immediate alert on first packet or state transition (zero <-> nonzero)
                            state_changed = (last_zero_status is not None and last_zero_status != iq_stats["is_all_zero"])
                            last_zero_status = iq_stats["is_all_zero"]

                            if ep6_audio_count == 1 or ep6_audio_count % 381 == 1 or state_changed:
                                if iq_stats["is_all_zero"]:
                                    print(f"[{now - start_time:7.2f}s] [PICO -> QUISK] EP6 Audio Seq={seq:8d} | [ALL ZEROS / SILENCE] 126/126 I/Q samples are 0x000000 (Noise floor at -180 dBFS)")
                                    print(f"                                      Raw first 16 bytes: {iq_stats['first_raw_hex']}")
                                else:
                                    print(f"[{now - start_time:7.2f}s] [PICO -> QUISK] EP6 Audio Seq={seq:8d} | [LIVE I/Q DATA] Peak={iq_stats['peak']} (I:{iq_stats['peak_i']}, Q:{iq_stats['peak_q']}), RMS={iq_stats['rms_dbfs']:.1f} dBFS")
                                    print(f"                                      First I/Q pairs: I={iq_stats['first_i']}, Q={iq_stats['first_q']}")
                                    print(f"                                      Raw first 16 bytes: {iq_stats['first_raw_hex']}")
                    else:
                        print(f"[{now - start_time:7.2f}s] [PICO -> QUISK] Unknown Packet: Len={len(data)}: {data[:16].hex()}")

                    if quisk_addr is not None:
                        sock_quisk.sendto(data, quisk_addr)

                elif s is sock_quisk:
                    # Packet received from Quisk -> Forward to Pico W
                    data, addr = sock_quisk.recvfrom(2048)
                    quisk_addr = addr
                    quisk_pkts += 1

                    if len(data) >= 3 and data[0] == 0xEF and data[1] == 0xFE and data[2] == 0x02:
                        discovery_count += 1
                        print(f"[{now - start_time:7.2f}s] [QUISK -> PICO] Discovery Probe (0xEFFE 0x02, Len={len(data)})")
                    elif len(data) >= 4 and data[0] == 0xEF and data[1] == 0xFE and data[2] == 0x04:
                        action = "START Rx" if data[3] == 0x01 else ("START Rx+Tx" if data[3] == 0x03 else ("STOP" if data[3] == 0x00 else f"0x{data[3]:02X}"))
                        if data[3] & 0x01:
                            start_cmd_count += 1
                        else:
                            stop_cmd_count += 1
                        print(f"[{now - start_time:7.2f}s] [QUISK -> PICO] Metis Run Command: {action} (0xEFFE 0x04 0x{data[3]:02X}, Len={len(data)})")
                    elif len(data) in (512, 1032) and data[0] == 0xEF and data[1] == 0xFE:
                        ep2_cc_count += 1
                        cmd_addr = (data[11] >> 1) & 0x3F if len(data) > 11 else 0
                        if cmd_addr == 1:
                            freq = (data[12] << 24) | (data[13] << 16) | (data[14] << 8) | data[15]
                            print(f"[{now - start_time:7.2f}s] [QUISK -> PICO] EP2 C&C: TUNE VFO -> {freq / 1e6:.6f} MHz ({freq} Hz)")
                        elif ep2_cc_count % 50 == 1:
                            print(f"[{now - start_time:7.2f}s] [QUISK -> PICO] EP2 C&C Heartbeat #{ep2_cc_count} (CmdAddr=0x{cmd_addr:02X})")
                    else:
                        print(f"[{now - start_time:7.2f}s] [QUISK -> PICO] Unrecognized Frame: Len={len(data)}: {data[:16].hex()}")

                    sock_pico.sendto(data, pico_target)

    except KeyboardInterrupt:
        print("\n" + "=" * 70)
        print(" OPENHPSDR PROXY & IQ SAMPLE SUMMARY")
        print("=" * 70)
        print(f"Quisk Packets Forwarded:      {quisk_pkts}")
        print(f"Pico W Packets Forwarded:     {pico_pkts}")
        print(f"EP6 Audio Frames Forwarded:   {ep6_audio_count}")
        print(f"  - Non-Zero (Live Signal):   {ep6_nonzero_frames} frames")
        print(f"  - All Zero (Silence):       {ep6_zero_frames} frames")
        if ep6_audio_count > 0:
            if ep6_nonzero_frames > 0:
                print("\n[RESULT] Live non-zero I/Q samples are reaching the host!")
            else:
                print("\n[RESULT: ZERO PAYLOAD] All received I/Q frames contained only 0x000000.")
                print("         This causes Quisk to calculate -inf dB (-180 dBFS line).")
                print("         Verify ADC DMA / I2S audio input is capturing samples on the Pico.")
        print("=" * 70)

if __name__ == "__main__":
    args = parse_args()
    run_proxy(args.pico_ip, args.pico_port, args.local_port)
