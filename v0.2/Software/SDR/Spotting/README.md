# WWU 2026 SDR Harvester and Spotting Pipeline

This directory contains a multi-radio receive pipeline for WWU 2026 SDR boards.

The C++ harvester opens one SoapySDR stream per configured radio, captures 96 kHz complex I/Q, translates each requested RF offset to a nonzero audio IF, removes DC, decimates to 12 kHz, and writes UTC-aligned mono WAV files.

The Python reporter watches completed WAV files, runs `jt9` or `wsprd`, parses spots, optionally submits them to PSKReporter, and removes processed or stale files from the capture directory.

The sample configuration enables FT8 and FT4. WSPR is supported but disabled in `config.json` because WSPR frames are 120 seconds long.

## Files

| File | Purpose |
|---|---|
| `config.json` | Capture directory, sample rates, radios, RF centers, mode offsets, and audio IFs |
| `harvester.cpp` | Capture, DSP, UTC framing, and WAV output |
| `CMakeLists.txt` | Builds the C++ harvester |
| `reporter.json` | Callsign/grid, decoder, cleanup, and PSKReporter settings |
| `reporter.py` | Decode, deduplicate, report, and cleanup daemon |
| `cleanup_capture.py` | Manual removal of old or abandoned capture files |
| `build/harvester` | Compiled executable produced by CMake |

The SoapySDR board driver is a separate project in `../Soapy-For-2026-Board` and must be installed first.

## Hardware prerequisites

Each board needs both interfaces available to Ubuntu:

1. USB CDC serial, normally `/dev/ttyACM0`, for firmware control and tuning.
2. USB ALSA capture audio for stereo I/Q.

Check the board with:

```sh
lsusb
ls -l /dev/ttyACM*
cat /proc/asound/cards
cat /proc/asound/cardN/stream0
SoapySDRUtil --find="driver=2026sdr"
```

The audio interface must list 96000 Hz. If it lists only 48000 Hz, set the board's MD1 jumper HIGH or install the 96 kHz firmware variant.

Verify the driver before running the harvester:

```sh
cd ../Soapy-For-2026-Board
python3 test_driver.py --rate 96000 --freq 14074000
```

The test should report `Actual rate: 96000 Hz` and `PASS`.

## Packages

On Ubuntu/Debian:

```sh
sudo apt update
sudo apt install \
  build-essential cmake pkg-config g++ \
  libsoapysdr-dev soapysdr-tools python3-soapysdr \
  libliquid-dev libsndfile1-dev nlohmann-json3-dev \
  libasound2-dev libusb-1.0-0-dev \
  wsjtx python3-watchdog
```

If `python3-watchdog` is unavailable:

```sh
python3 -m pip install --user watchdog
```

Check the required programs and Python import:

```sh
command -v cmake jt9 wsprd SoapySDRUtil
python3 -c 'import watchdog; print("watchdog OK")'
```

The current reporter uses Python's standard library plus `watchdog`. It does not require `requests`.

## Build

From this directory:

```sh
cmake -S . -B build
cmake --build build -j4
```

Re-run the build after changing `harvester.cpp` or `CMakeLists.txt`.

## Configure one radio

Example `config.json`:

```json
{
  "output_dir": "/home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting/captures",
  "sample_rate": 96000,
  "output_rate": 12000,
  "sdrs": [
    {
      "sdr_id": "Board_1",
      "serial_port": "/dev/ttyACM0",
      "audio_label": "S2026",
      "center_freq": 14074000,
      "modes": [
        { "mode": "FT8", "offset": 0, "audio_if": 1500 },
        { "mode": "FT4", "offset": 6000, "audio_if": 1500 }
      ]
    }
  ]
}
```

Meanings:

- `output_dir` is the directory used for temporary and completed WAV files. The current configuration uses persistent storage under the project directory.
- `sdr_id` is unique and becomes part of every WAV filename.
- `serial_port` selects the board's CDC port.
- `audio_label` is a case-insensitive ALSA-card search string.
- `center_freq` is the programmed RF center in Hz.
- A positive `offset` means the target RF signal is `center_freq + offset`.
- `audio_if` is the target frequency in the 12 kHz WAV. Use a nonzero value such as 1500 Hz to avoid DC and 1/f noise.
- The NCO frequency is `(audio_if - offset) / 96000`.

The current DSP chain includes a DC blocker, a 0.05 normalized anti-alias filter, decimation by 8, and monitoring gain in the WAV writer. Watch for clipping if changing gain.

Use `output_dir` in both JSON files whenever the capture location changes.

## Configure multiple radios

Add one object per board to the `sdrs` array. Each entry needs a unique `sdr_id`, serial port, and ALSA label:

```json
{
  "sdr_id": "Board_2",
  "serial_port": "/dev/ttyACM1",
  "audio_label": "S2026_1",
  "center_freq": 21107000,
  "modes": [
    { "mode": "FT8", "offset": -33000, "audio_if": 1500 },
    { "mode": "FT4", "offset": 33000, "audio_if": 1500 }
  ]
}
```

This example puts 15 m FT8 at 21.074 MHz and 15 m FT4 at 21.140 MHz in the same 96 kHz capture window. Use udev rules for stable serial names when possible. Inspect `/proc/asound/cards` after both boards are plugged in and replace `/dev/ttyACM1` and `S2026_1` with the actual second-board identifiers. Do not map two entries to the same serial device or ALSA card.

The harvester creates one thread and one SoapySDR stream per `sdrs` entry. Example output names:

```text
Board_1_FT8_20260813T001500Z.wav
Board_2_FT8_20260813T001500Z.wav
```

### Missing or mismatched radio behavior

Each radio worker is isolated. If a configured serial port, ALSA label, or Soapy stream cannot be opened, the harvester logs the radio ID and retries that radio every five seconds. Other radios continue capturing, and the missing radio can recover automatically when it is connected later. A failed or disconnected radio does not publish an incomplete WAV frame; any `.wav.part` file for that frame is discarded.

The `/dev/ttyACM0`, `/dev/ttyACM1`, and similar numbers are assigned by Ubuntu and can change when boards are unplugged or reconnected. A path that exists may still refer to the wrong physical board. For reliable multi-radio operation, create stable udev symlinks based on each board's USB identity and use those symlinks in `config.json`. Also verify that every board has a distinct ALSA `audio_label`.

## Manual Si5351 tuning

The planned JSON shape for manual PLL/register values is:

```json
"si5351": {
  "si5351_hz": 7074000,
  "N": 86,
  "a": 24,
  "b": 0,
  "c": 1,
  "p1": 2560,
  "p2": 0,
  "p3": 1
}
```

The current harvester does not yet pass this object to `setFrequency()`. Until the Soapy driver and harvester are updated together, the driver calculates Si5351 parameters automatically. Do not use the example values blindly.

## Configure reporter.json

Edit the receiver identity:

```json
{
  "output_dir": "/home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting/captures",
  "harvester_config": "config.json",
  "callsign": "KL7NA",
  "grid": "DN06",
  "decode_timeout": 180,
  "stale_file_seconds": 900,
  "cleanup_interval_seconds": 60,
  "pskreporter": {
    "enabled": true,
    "host": "report.pskreporter.info",
    "port": 4739,
    "identifier": 2026
  }
}
```

Keep PSKReporter disabled until local decoding works. When enabled, `reporter.py` sends UDP packets to `report.pskreporter.info:4739`.

The reporter reads `config.json` through `harvester_config`, so it can reconstruct RF frequency from each WAV:

```text
RF frequency = center_freq + offset + decoded_audio_frequency - audio_if
```

## Start the pipeline

Start the reporter first:

```sh
cd /home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting
python3 reporter.py --config reporter.json
```

In a second terminal, start the harvester:

```sh
cd /home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting
./build/harvester config.json
```

The harvester writes `.wav.part` while a frame is being written and atomically renames it to `.wav` when complete. The reporter handles both normal creation and atomic move events. It also processes existing WAV files when it starts.

Stop either daemon with `Ctrl-C`. The harvester does not publish incomplete frames.

## Capture storage

Each frame is a short, sequential 12 kHz WAV file, and the reporter normally deletes it immediately after decoding. A local SSD is the recommended default; an older spinning disk should also work because the data rate is low and decode latency is not critical.

The current configuration uses:

```text
/home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting/captures
```

Create it once if necessary:

```sh
mkdir -p /home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting/captures
```

If the storage location changes, update `output_dir` in both `config.json` and `reporter.json` to the same writable directory.

## Run automatically with systemd

The recommended setup uses per-user systemd services. This keeps the daemons running as your normal user, which is important for access to the USB serial and ALSA devices.

Create the user-service directory:

```sh
mkdir -p ~/.config/systemd/user
```

Create `~/.config/systemd/user/wwu2026-reporter.service`:

```ini
[Unit]
Description=WWU 2026 SDR spot reporter
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting
ExecStart=/usr/bin/python3 /home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting/reporter.py --config /home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting/reporter.json
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
```

Create `~/.config/systemd/user/wwu2026-harvester.service`:

```ini
[Unit]
Description=WWU 2026 SDR harvester
After=wwu2026-reporter.service
Wants=wwu2026-reporter.service

[Service]
Type=simple
WorkingDirectory=/home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting
ExecStart=/home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting/build/harvester /home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting/config.json
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
```

Reload, enable, and start both services:

```sh
systemctl --user daemon-reload
systemctl --user enable --now wwu2026-reporter.service
systemctl --user enable --now wwu2026-harvester.service
```

Check status and follow logs:

```sh
systemctl --user status wwu2026-reporter.service
systemctl --user status wwu2026-harvester.service
journalctl --user -u wwu2026-reporter.service -f
journalctl --user -u wwu2026-harvester.service -f
```

Stop or disable them:

```sh
systemctl --user disable --now wwu2026-harvester.service wwu2026-reporter.service
```

For services to start even when you are not logged into a graphical session, enable user lingering once:

```sh
sudo loginctl enable-linger frohro
```

If the harvester cannot open `/dev/ttyACM0` or the ALSA device under systemd, verify that your user has the required `dialout` and `audio` group membership, then log out and back in:

```sh
groups
sudo usermod -aG dialout,audio frohro
```

## What the scripts do

### `harvester.cpp`

For every configured radio it:

1. Creates a `2026sdr` Soapy device.
2. Sets 96 kHz capture and the configured RF center.
3. Creates an NCO and FIR decimator branch for every requested mode.
4. Removes DC and translates the requested RF offset to `audio_if`.
5. Decimates 96 kHz to 12 kHz.
6. Writes real-valued 16-bit mono WAV frames aligned to UTC boundaries.

FT8 frames are 15 seconds, FT4 frames are 7.5 seconds, and WSPR frames are 120 seconds.

### `reporter.py`

For each complete WAV it:

1. Identifies FT8, FT4, or WSPR from the filename.
2. Runs `jt9 -8`, `jt9 -5`, or `wsprd`.
3. Parses callsign, grid, SNR, and audio frequency.
4. Suppresses duplicate `(frame, mode, callsign, frequency)` spots.
5. Logs accepted spots.
6. Optionally sends them to PSKReporter.
7. Deletes the WAV after decoding.

If decoding fails or times out, the WAV remains until stale-file cleanup removes it.

### `cleanup_capture.py`

Remove only files older than 15 minutes:

```sh
python3 cleanup_capture.py --directory /home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting/captures --older-than 900
```

Remove all WAV and partial files after stopping both daemons:

```sh
python3 cleanup_capture.py --directory /home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting/captures --all
```

The reporter also performs automatic cleanup according to `stale_file_seconds` and `cleanup_interval_seconds`.

## Testing without an antenna

Do not run the reporter for this test if you want to keep the WAV files:

```sh
python3 cleanup_capture.py --directory /home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting/captures --all
timeout --signal=INT 40s ./build/harvester config.json
find /home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting/captures -maxdepth 1 -type f -name '*.wav' -printf '%f %s bytes\n' | sort
file /home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting/captures/*.wav
```

You can listen with:

```sh
aplay /home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting/captures/Board_1_FT8_*.wav
```

No antenna normally produces thermal noise and receiver artifacts rather than decodes.

## Troubleshooting

### No Soapy device

```sh
SoapySDRUtil --find="driver=2026sdr"
SoapySDRUtil --make="driver=2026sdr,serial_port=/dev/ttyACM0,audio_label=S2026"
```

Check udev permissions, `/dev/ttyACM*`, `/proc/asound/cards`, and `SoapySDRUtil --info`.

### 96 kHz fails

```sh
cat /proc/asound/cardN/stream0
```

The capture interface must list a 96000 Hz alternate setting. Check the MD1 jumper or board firmware.

### No WAV files

Run the harvester in the foreground and verify the capture directory:

```sh
mkdir -p /home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting/captures
test -w /home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting/captures && echo writable
```

### WAV files but no spots

Run a decoder manually:

```sh
jt9 -8 /home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting/captures/Board_1_FT8_YYYYMMDDTHHMMSSZ.wav
jt9 -5 /home/frohro/Projects/Intro-to-CAD-2026/v0.2/Software/SDR/Spotting/captures/Board_1_FT4_YYYYMMDDTHHMMSSZ.wav
```

Check mode duration, 12 kHz sample rate, UTC alignment, audio IF, antenna, and RF frequency.

### Local spots but no PSKReporter spots

Set `pskreporter.enabled` to `true`, verify callsign/grid, and check outbound UDP access to `report.pskreporter.info:4739`.

## Time alignment

Frame boundaries use the host system clock in UTC. Keep Ubuntu synchronized with chrony or systemd-timesyncd. The Soapy driver supplies no hardware timestamps, so host-clock timing cannot correct USB/audio-clock latency.
