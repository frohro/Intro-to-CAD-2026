# Quisk OpenHPSDR Configuration for Intro-to-CAD-2026 / Pico W SDR
#
# Usage:
#   quisk -c quisk_conf_openhpsdr.py -v
#
from __future__ import print_function, absolute_import, division
import os, sys

# --------------------------------------------------------------------------
# 1. Base Hardware Class with Safe Widget Stubs
# --------------------------------------------------------------------------
driver_name = "unknown"
try:
    from hermes.quisk_hardware import Hardware as HermesHardware
    driver_name = "hermes.quisk_hardware (Native C Backend)"
except ImportError:
    try:
        from quisk_hardware_hermes import Hardware as HermesHardware
        driver_name = "quisk_hardware_hermes"
    except ImportError:
        from quisk_hardware_model import Hardware as HermesHardware
        driver_name = "quisk_hardware_model (Warning: Fallback Model)"

print(f"[*] Quisk loaded hardware driver: {driver_name}")

class Hardware(HermesHardware):
    """
    Subclass Quisk's native Hermes hardware driver to stub out missing 
    daughterboards (Alex filter bank, step attenuators, PA relays) 
    so Quisk's GUI widgets will not crash.
    """
    def __init__(self, app, conf):
        self._last_diag_time = 0
        try:
            super(Hardware, self).__init__(app, conf)
            print("[*] Quisk Hardware initialized successfully.")
        except Exception as e:
            print(f"[!] Hardware init exception: {e}")

    def HeartBeat(self):
        """Called periodically by Quisk GUI timer (~10-20 Hz)."""
        try:
            if hasattr(super(Hardware, self), 'HeartBeat'):
                return super(Hardware, self).HeartBeat()
            elif hasattr(super(Hardware, self), 'heartbeat'):
                return super(Hardware, self).heartbeat()
        except Exception as e:
            pass
        return None

    def heartbeat(self):
        return self.HeartBeat()

    # Safe stubs for peripheral widgets not present on the Pico W board
    def set_attenuation(self, att):
        pass

    def set_preamp(self, preamp):
        pass

    def set_filter(self, filter_num):
        pass

    def set_alex(self, val):
        pass

    def set_tx_power(self, power):
        pass

    def set_cw_key(self, state):
        pass

    def get_tx_power(self):
        return 0.0

    def get_swr(self):
        return 1.0

    def open(self):
        """Catch any driver open issues smoothly."""
        try:
            return super(Hardware, self).open()
        except Exception as e:
            print(f"[!] Hardware.open() exception: {e}")
            return "Pico W Open"

    def close(self):
        """Clean closure."""
        try:
            return super(Hardware, self).close()
        except Exception as e:
            return None

    def ChangeFrequency(self, tune, vfo, source='', band='', event=None):
        """Safe tuning handler."""
        try:
            return super(Hardware, self).ChangeFrequency(tune, vfo, source=source, band=band, event=event)
        except TypeError:
            try:
                return super(Hardware, self).ChangeFrequency(tune, vfo)
            except Exception as e:
                print(f"[!] ChangeFrequency error: {e}")
                return tune, vfo
        except Exception as e:
            print(f"[!] ChangeFrequency error: {e}")
            return tune, vfo

# --------------------------------------------------------------------------
# 2. Network & Radio Settings
# --------------------------------------------------------------------------
# Set Pico W IP address for direct connection (if broadcast is blocked)
hermes_ip = "192.168.1.186"
hermes_port = 1024

# Sound: Quisk OpenHPSDR receives 24-bit I/Q over network (UDP).
# Audio Playback uses ALSA directly to prevent PulseAudio re-init crashes on toggle
name_of_sound_capt = ""
name_of_sound_play = "alsa:default"

sample_rate = 48000
openradio_lower = 3_500_000
openradio_upper = 30_000_000
default_frequency = 7050000

# Use custom clean widget module tailored for Pico W receiver
widget_file_path = "quisk_widgets_picow.py"

# --------------------------------------------------------------------------
# 3. Default Display & Audio Settings
# --------------------------------------------------------------------------
display_waterfall = 1
display_graph = 1

# Initial Volume level (0.0 to 1.0)
rx_audio_volume = 0.7
volume = 0.7

# Display scaling for clean spectrum visibility
graph_min = -140
graph_max = -20


