#!/usr/bin/env python3
import argparse, json, logging, re, socket, struct, subprocess, time
from pathlib import Path
from watchdog.events import FileSystemEventHandler
from watchdog.observers import Observer

LOG = logging.getLogger("wwu2026-reporter")
DECODE = re.compile(r"^\s*\d{6}\s+(-?\d+)\s+([0-9.]+)\s+(\d+)\s+(.+?)\s*$")
CALL = re.compile(r"\b[A-Z0-9]{3,}(?:/[A-Z0-9]+)?\b")
GRID = re.compile(r"\b[A-Ra-r]{2}\d{2}(?:[A-Xa-x]{2})?\b")

def parse_output(text):
    spots = []
    for line in text.splitlines():
        m = DECODE.match(line)
        if not m: continue
        snr, dt, freq, message = int(m[1]), float(m[2]), int(m[3]), m[4]
        calls = CALL.findall(message.upper()); grids = GRID.findall(message)
        if calls: spots.append({"call": calls[0], "grid": grids[0].upper() if grids else "", "snr": snr, "freq": freq, "dt": dt})
    return spots

def field(value):
    raw = value.encode("ascii", "ignore")[:254]; return bytes([len(raw)]) + raw
def block(marker, body):
    body += b"\0" * ((-len(body)) % 4); return struct.pack("!HH", marker, len(body) + 4) + body

# PSK Reporter IPFIX record-format descriptors for the fields emitted below.
RECEIVER_DESC = bytes.fromhex("000300249992000300018002ffff0000768f8004ffff0000768f8008ffff0000768f0000")
SENDER_DESC = bytes.fromhex("0002002c999300058001ffff0000768f800500040000768f800affff0000768f800b00010000768f00960004")

class Reporter:
    def __init__(self, cfg):
        self.cfg, self.seen, self.seq = cfg, set(), 0
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # output_dir is persistent storage by default. Keep accepting the old
        # ramdisk key so existing reporter configurations continue to work.
        self.ramdisk = Path(cfg.get("output_dir", cfg.get("ramdisk", "/home/frohro/SDR/Spotting/captures")))
        self.ramdisk.mkdir(parents=True, exist_ok=True)
        self.stale_seconds = int(cfg.get("stale_file_seconds", 900))
        self.cleanup_interval = int(cfg.get("cleanup_interval_seconds", 60))
        self.last_cleanup = 0.0

    def mode_from_path(self, path):
        name = path.name.upper()
        return next((m for m in ("FT8", "FT4", "WSPR") if m in name), None)

    def cleanup(self, force=False):
        now = time.time()
        if not force and now - self.last_cleanup < self.cleanup_interval: return
        self.last_cleanup = now
        for path in self.ramdisk.iterdir():
            if not path.is_file() or not (path.name.endswith(".wav") or path.name.endswith(".wav.part")): continue
            try:
                age = now - path.stat().st_mtime
                if age >= self.stale_seconds:
                    path.unlink(); LOG.warning("removed stale file %s (%.0fs old)", path, age)
            except FileNotFoundError: pass
            except OSError: LOG.exception("could not remove stale file %s", path)

    def radio_for_path(self, path):
        # Filename format: Board_1_FT8_20260812T223330Z.wav
        # The mode token makes the board prefix unambiguous even when the
        # sdr_id itself contains underscores.
        match = re.match(r"^(?P<board>.+)_(?:FT8|FT4|WSPR)_", path.name, re.IGNORECASE)
        board = match.group("board") if match else path.stem
        for radio in self.cfg.get("radios", []):
            if radio.get("sdr_id") == board: return radio
        return {"center_freq": int(self.cfg.get("frequency_hz", 0)), "modes": []}

    def frequency_from_path(self, path, mode, audio_frequency):
        radio = self.radio_for_path(path)
        mode_cfg = next((m for m in radio.get("modes", []) if m.get("mode", "").upper() == mode), {})
        # jt9/wsprd report the frequency in the 12-kHz audio file. Convert
        # that audio frequency back to RF using the configured audio IF and
        # physical RF offset.
        audio_if = int(mode_cfg.get("audio_if", 1500))
        rf_offset = int(mode_cfg.get("offset", 0))
        return int(radio.get("center_freq", 0)) + rf_offset + int(audio_frequency) - audio_if

    def process(self, path):
        if not path.exists() or path.suffix != ".wav": return
        mode = self.mode_from_path(path)
        if not mode: return
        if mode == "WSPR": command = [self.cfg.get("wsprd", "wsprd"), str(path)]
        else: command = [self.cfg.get("jt9", "jt9"), "-8" if mode == "FT8" else "-5", str(path)]
        try: result = subprocess.run(command, capture_output=True, text=True, timeout=self.cfg.get("decode_timeout", 180), check=False)
        except Exception: LOG.exception("decoder failed for %s", path); return
        window = path.stem.rsplit("_", 1)[-1]
        for spot in parse_output(result.stdout + "\n" + result.stderr):
            spot["path"] = path
            key = (window, mode, spot["call"], spot["freq"])
            if key in self.seen: continue
            self.seen.add(key); self.report(mode, spot)
        try: path.unlink()
        except FileNotFoundError: pass
    def report(self, mode, spot):
        frequency = self.frequency_from_path(spot.get("path", Path("")), mode, spot["freq"])
        LOG.info("%s %s grid=%s audio=%s Hz RF=%s Hz snr=%s", mode, spot["call"], spot["grid"], spot["freq"], frequency, spot["snr"])
        p = self.cfg.get("pskreporter", {})
        if not p.get("enabled", False): return
        receiver = block(0x9992, field(p.get("callsign", self.cfg.get("callsign", "YOUR_CALLSIGN"))) + field(p.get("grid", self.cfg.get("grid", "AA00aa"))) + field("WWU2026 Spotting"))
        sender = block(0x9993, field(spot["call"]) + struct.pack("!I", max(0, frequency)) + field(mode) + b"\1" + struct.pack("!I", int(time.time())))
        body = RECEIVER_DESC + SENDER_DESC + receiver + sender
        packet = struct.pack("!HHIII", 10, 16 + len(body), int(time.time()), self.seq, p.get("identifier", 2026)) + body
        self.sock.sendto(packet, (p.get("host", "report.pskreporter.info"), p.get("port", 4739))); self.seq += 1

class Handler(FileSystemEventHandler):
    def __init__(self, reporter): self.reporter = reporter
    def on_created(self, event):
        if not event.is_directory and event.src_path.endswith(".wav"): self.reporter.process(Path(event.src_path))
    def on_moved(self, event):
        # The harvester atomically renames frame.wav.part to frame.wav;
        # inotify/watchdog reports that publication as a move event.
        if not event.is_directory and event.dest_path.endswith(".wav"): self.reporter.process(Path(event.dest_path))

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("-c", "--config", default="reporter.json"); args = ap.parse_args()
    with open(args.config) as f: cfg = json.load(f)
    harvester_config = cfg.get("harvester_config", "config.json")
    if harvester_config:
        try:
            with open(harvester_config) as f: cfg["radios"] = json.load(f).get("sdrs", [])
        except OSError: LOG.warning("could not load harvester config %s; RF reports use fallback frequency", harvester_config)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    reporter = Reporter(cfg)
    reporter.cleanup(force=True)
    # Recover completed WAVs that were created while the reporter was stopped.
    for path in sorted(reporter.ramdisk.glob("*.wav")): reporter.process(path)
    observer = Observer(); observer.schedule(Handler(reporter), str(reporter.ramdisk), recursive=False); observer.start()
    try:
        while True:
            reporter.cleanup(); time.sleep(1)
    except KeyboardInterrupt: observer.stop()
    observer.join()
if __name__ == "__main__": main()
