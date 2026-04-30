# Quisk configuration for Cadyn's Pico SDR board
#
# Copy this file to your Quisk configuration directory and point Quisk to it:
#   quisk --conf /path/to/quisk_conf_cadyn.py
#
# Hardware: Cadyn's hand-wired Pico SDR (reference build)
#   - Raspberry Pi Pico via USB CDC serial (/dev/ttyACM0 or /dev/ttyACM1)
#   - 3.5 mm stereo I/Q audio to soundcard (PulseAudio)
#   - 48 kHz sample rate

from __future__ import print_function
from __future__ import absolute_import
from __future__ import division

# ---- Sound card settings -----------------------------------------------
# Use PulseAudio. Change to "portaudio:(hw:X,Y)" if you prefer direct ALSA.
name_of_sound_capt = "pulse"
name_of_sound_play = "pulse"

# Sample rate of the external soundcard.
# All golden-frequency search thresholds are derived from this value.
sample_rate = 48000

# ---- Frequency limits --------------------------------------------------
openradio_lower = 3_800_000
openradio_upper = 30_000_000

# ---- Hardware control class --------------------------------------------
import math
import serial
import time
from quisk_hardware_model import Hardware as BaseHardware

class Hardware(BaseHardware):
    def open(self):
        """Open the serial connection to the Pico and configure it."""
        baud = 115200
        # Try ttyACM0 first (MicroPython USB CDC), fall back to ttyACM1.
        for port in ("/dev/ttyACM0", "/dev/ttyACM1"):
            try:
                self.or_serial = serial.Serial(port, baud, timeout=3)
                print("Opened", port)
                break
            except serial.serialutil.SerialException:
                continue
        else:
            raise serial.serialutil.SerialException(
                "Pico not found on /dev/ttyACM0 or /dev/ttyACM1")

        # Give MicroPython a moment to finish its boot message.
        time.sleep(2)

        # Confirm firmware identity.
        version = str(self._get_parameter("VER"))
        print("Pico firmware:", version)

        # Query crystal reference frequency — used for all golden-LO searches.
        xtal_raw = self._get_parameter("XTAL")
        try:
            self._crystal_freq = float(xtal_raw)
        except (ValueError, TypeError):
            self._crystal_freq = 24_576_000.0   # safe default
        print("Crystal freq: %.3f Hz" % self._crystal_freq)

        # Tell the Pico the soundcard sample rate (still used for NeoPixel
        # threshold for out-of-spec fallback detection).
        self._set_parameter("RATE", str(sample_rate))

        self._golden_status = "PLL: ready"
        self._last_lo       = None   # last programmed LO frequency (Hz)

        t = version + ". Capture from %s at %d Hz." % (
            self.conf.name_of_sound_capt, sample_rate)
        return t

    def close(self):
        self.or_serial.close()

    def _find_golden_los(self, tune):
        """Return sorted list of all golden LO frequencies near tune.

        A golden LO is one where the Si5351 PLL feedback multiplier M is an
        integer, giving zero fractional spurs.  Each entry is (lo_freq, signed_offset)
        where signed_offset = lo_freq - tune.

        Only LOs that keep tune inside the audio passband are returned:
            |signed_offset| < half_bw = sample_rate / 2

        The list is sorted by sweet-spot cost:
            cost = | |signed_offset| - sample_rate/4 |
        so index 0 is the best initial placement (DC spike at the passband
        edge, not near DC or near tune).
        """
        half_bw = sample_rate // 2
        crystal = self._crystal_freq
        N_min = max(4,   int(math.ceil(600_000_000.0 / tune)))
        N_max = min(127, int(900_000_000.0 / tune))
        quarter = sample_rate // 4
        results = []
        for N in range(N_min, N_max + 1):
            M_exact = tune * N / crystal
            if not (14 < M_exact < 91):
                continue
            M_int = round(M_exact)
            lo = crystal * M_int / N
            signed = lo - tune
            if abs(signed) < half_bw:
                cost = abs(abs(signed) - quarter)
                results.append((cost, lo, signed))
        results.sort()
        return [(lo, signed) for (cost, lo, signed) in results]

    def _program_lo(self, lo_hz):
        """Send FREQ,<lo_hz> to the Pico and parse its response.

        Returns signed_offset (f_lo - f_requested) as reported by the Pico.
        Updates self._golden_status and self._last_lo.
        """
        self._send("FREQ," + str(int(round(lo_hz))))
        self._readline()                          # discard echoed frequency
        ok_line = self._readline().decode(errors='replace').strip()
        self._last_lo = lo_hz
        parts = ok_line.split(",")
        signed_offset = 0
        if len(parts) >= 3 and parts[0] == "OK":
            ptype = parts[1]
            try:
                signed_offset = int(parts[2])
            except ValueError:
                signed_offset = 0
            if ptype == "G":
                self._golden_status = (
                    "GOLDEN  {:+d} Hz  (integer PLL \u2014 no spurs)".format(signed_offset)
                )
            elif ptype == "F":
                self._golden_status = "frac  (exact freq)"
            else:
                self._golden_status = "fallback  (VCO out of spec)"
        return signed_offset

    def ChangeFrequency(self, tune, vfo, source='', band='', event=None):
        """Called by Quisk for every VFO or tune change, including arrow presses.

        Design
        ------
        We treat tune as the RF frequency of the station being received and
        choose the LO (= vfo sent to the Pico) independently of what Quisk
        passed as vfo.  The returned (tune, actual_lo) pair tells Quisk where
        we actually placed the LO so its frequency display is correct.

        Arrow press detection
        ---------------------
        If vfo moved away from self._last_lo (by a step Quisk generated), that
        signals an arrow press.  direction = sign(vfo - last_lo).
        We search for the nearest golden LO in that direction that keeps tune
        inside the passband (|lo - tune| < half_bw).  If none exists on that
        side, we fall back to a plain ±sample_rate/4 fractional shift.

        Normal tune (digit click, band change, startup)
        ------------------------------------------------
        When tune itself changed (or there is no previous LO), we pick the
        golden LO whose |offset| is closest to sample_rate/4 — placing the
        DC spike near the passband edge, away from DC and away from the signal.
        """
        tune = max(openradio_lower, min(openradio_upper, tune))
        half_bw = sample_rate // 2
        quarter = sample_rate // 4

        golden = self._find_golden_los(tune)

        # ---- Detect arrow press vs normal tune ----
        # An arrow press shifts vfo but leaves tune unchanged; the vfo step
        # comes from Quisk's own IF-shift button, typically sample_rate//4.
        is_arrow = (self._last_lo is not None and
                    abs(vfo - self._last_lo) > 100 and    # not just rounding
                    abs(vfo - self._last_lo) < half_bw)   # not a big tune jump

        if is_arrow:
            direction = 1 if vfo > self._last_lo else -1
            # Find the nearest golden LO in the requested direction.
            # "Nearest" = smallest |lo - last_lo| among candidates on that side.
            candidates = [(abs(lo - self._last_lo), lo, signed)
                          for (lo, signed) in golden
                          if (lo - self._last_lo) * direction > 0]
            if candidates:
                candidates.sort()
                chosen_lo = candidates[0][1]
            else:
                # No golden in that direction — fractional fallback:
                # shift LO by quarter in the requested direction, clamped so
                # tune stays inside the passband.
                shift = direction * quarter
                new_lo = self._last_lo + shift
                # Clamp: keep |new_lo - tune| < half_bw - small guard
                guard = 2000
                new_lo = max(tune - half_bw + guard,
                             min(tune + half_bw - guard, new_lo))
                chosen_lo = new_lo
        else:
            # Normal tune: sweet-spot golden (index 0) or fractional fallback.
            if golden:
                chosen_lo = golden[0][0]
            else:
                # No golden near tune — place LO at tune - quarter (DC spike
                # appears at +quarter in audio, away from DC and from tune).
                chosen_lo = tune - quarter

        chosen_lo = max(openradio_lower, min(openradio_upper, chosen_lo))
        self._program_lo(chosen_lo)
        return tune, chosen_lo

    def HeartBeat(self):
        """Called by Quisk ~10 times/second; push PLL status to the status bar."""
        try:
            self.app.StatusScreen(self._golden_status)
        except Exception:
            pass   # Quisk version without StatusScreen — NeoPixel still works

    # ---- Low-level serial helpers ----------------------------------------
    def _send(self, line):
        self.or_serial.write((line + "\n").encode())

    def _readline(self):
        return self.or_serial.readline()

    def _get_parameter(self, cmd):
        """Send cmd, return the argument from the echoed response line."""
        self._send(cmd)
        return self._get_argument()

    def _set_parameter(self, cmd, arg):
        """Send cmd,arg and wait for OK response."""
        self._send(cmd + "," + arg)
        # The Pico echoes the value then prints OK.
        self._get_argument()   # consume the echo line (or OK)
        return True

    def _get_argument(self):
        data = self._readline()
        if len(data) == 0:
            return -1
        if data.startswith(b'OK'):
            data = self._readline()
        if data.find(b',') == -1:
            return -1
        data = data.split(b',')[1].rstrip(b'\r\n')
        # Consume trailing OK line
        ok = self._readline()
        if ok.startswith(b'OK'):
            return data
        return data
