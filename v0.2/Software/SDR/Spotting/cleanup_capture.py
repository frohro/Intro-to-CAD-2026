#!/usr/bin/env python3
"""Remove completed or abandoned harvester files from the capture directory.

By default only files older than --older-than seconds are removed. Use
--all only when the harvester and reporter are stopped.
"""
import argparse, time
from pathlib import Path

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--directory", default="/home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting/captures",
                    help="directory containing harvester WAV files")
    ap.add_argument("--older-than", type=int, default=900)
    ap.add_argument("--all", action="store_true", help="remove all .wav and .wav.part files")
    args = ap.parse_args()
    root = Path(args.directory); now = time.time(); removed = 0
    for path in root.iterdir():
        if not path.is_file() or not (path.name.endswith(".wav") or path.name.endswith(".wav.part")): continue
        if not args.all and now - path.stat().st_mtime < args.older_than: continue
        path.unlink(); removed += 1; print(path)
    print(f"removed {removed} file(s) from {root}")

if __name__ == "__main__": main()
