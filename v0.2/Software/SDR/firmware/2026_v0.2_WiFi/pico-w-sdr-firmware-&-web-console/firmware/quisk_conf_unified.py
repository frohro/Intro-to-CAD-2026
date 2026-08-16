# Updated Quisk configuration for Intro-to-CAD-2026 / Pico W SDR
#
# Supports both:
#   1. USB Mode: ALSA "hw:S2026,0" audio + USB CDC Control (/dev/ttyACM0)
#   2. WiFi Mode: OpenHPSDR Protocol 1 (UDP 1024) or TCP Control (Port 5000)

from __future__ import print_function, absolute_import, division
import os, math, fractions, serial, serial.tools.list_ports, time
from quisk_hardware_model import Hardware as BaseHardware

name_of_sound_capt = "alsa:SDR PCM1808 2026 (hw:S2026,0)"
name_of_sound_play = "pulse"

sample_rate = 48000
openradio_lower = 3_800_000
openradio_upper = 30_000_000

class Hardware(BaseHardware):
    def open(self):
        baud = 115200
        port_device = None
        for info in serial.tools.list_ports.comports():
            if (info.vid, info.pid) in ((0xcafe, 0x4011), (0xcafe, 0x4010), (0xcafe, 0x4080)):
                port_device = info.device
                break
        if port_device is None:
            for p in ("/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyACM2"):
                if os.path.exists(p):
                    port_device = p
                    break

        if port_device is None:
            print("Pico W not detected over USB. If using WiFi, select OpenHPSDR in Quisk hardware settings.")
            raise serial.serialutil.SerialException("Pico W USB device not found")

        self.or_serial = serial.Serial(port_device, baud, timeout=3)
        self.or_serial.write(b'\x03')
        time.sleep(0.1)
        self.or_serial.write(b'\x04')
        self.or_serial.reset_input_buffer()

        self.or_serial.timeout = 0.2
        deadline = time.time() + 6.0
        while time.time() < deadline:
            line = self.or_serial.readline()
            if b'SDR ready' in line: break
        self.or_serial.timeout = 3

        version = str(self._get_parameter("VER"))
        print("Pico W SDR firmware:", version)
        self._set_parameter("RATE", str(sample_rate))
        return version

    def close(self):
        if hasattr(self, 'or_serial') and self.or_serial:
            self.or_serial.close()

    def ChangeFrequency(self, tune, vfo, source='', band='', event=None):
        tune = max(openradio_lower, min(openradio_upper, tune))
        # Send simple FREQ,<tune> command -- the Pico W computes the best Golden Integer LO on-board!
        self._send(f"FREQ,{int(round(tune))}")
        ack = self._readline().decode(errors='replace').strip()
        return int(tune), int(tune)

    def _send(self, line):
        self.or_serial.write((line + "\n").encode())

    def _readline(self):
        return self.or_serial.readline()

    def _get_parameter(self, cmd):
        self._send(cmd)
        data = self._readline().decode(errors='replace').strip()
        if ',' in data: return data.split(',')[1]
        return data

    def _set_parameter(self, cmd, arg):
        self._send(f"{cmd},{arg}")
        self._readline()
        return True
