# Quisk OpenHPSDR Configuration for Intro-to-CAD-2026 / Pico W SDR
#
# Usage:
#   quisk -c quisk_conf_openhpsdr.py -v
#
from __future__ import print_function, absolute_import, division
import os, sys

# Use dedicated Pico W Hardware driver (suppresses Hermes-Lite 2 PA/Alex/Attenuators)
try:
    from quisk_hardware_picow import Hardware
    driver_name = "quisk_hardware_picow (Pico W Unified Driver)"
except ImportError:
    try:
        from hermes.quisk_hardware import Hardware
        driver_name = "hermes.quisk_hardware"
    except ImportError:
        from quisk_hardware_model import Hardware
        driver_name = "quisk_hardware_model"

print(f"[*] Quisk loaded hardware driver: {driver_name}")

# --------------------------------------------------------------------------
# Network & Radio Settings
# --------------------------------------------------------------------------
hermes_ip = "192.168.1.186"
hermes_port = 1024

# Sound: Quisk OpenHPSDR receives 24-bit I/Q over UDP network.
# Playback defaults to standard system ALSA output.
name_of_sound_capt = ""
name_of_sound_play = "alsa:default"

# Default Sample Rate: 48000 or 96000
sample_rate = 96000
openradio_lower = 3_500_000
openradio_upper = 30_000_000
default_frequency = 7050000

# Use custom clean widget module tailored for Pico W receiver
widget_file_path = "quisk_widgets_picow.py"

# --------------------------------------------------------------------------
# Default Display & Audio Settings
# --------------------------------------------------------------------------
display_waterfall = 1
display_graph = 1

# Initial Volume level (0.0 to 1.0)
rx_audio_volume = 0.7
volume = 0.7

# Display scaling for clean spectrum visibility
graph_min = -140
graph_max = -20
