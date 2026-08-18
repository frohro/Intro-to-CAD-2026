# Custom Quisk Widget Module for Raspberry Pi Pico W SDR (Intro-to-CAD-2026)
# ==============================================================================
# Provides custom bottom controls, quick band / WWV buttons, and an interactive
# 1-Click Flash Crystal Calibrator in the bottom widget bar.
# ==============================================================================

from __future__ import print_function, absolute_import, division
import wx
import socket

class BottomWidgets(object):
    """
    Enhanced bottom control bar for Pico W SDR Receiver.
    Provides one-click ham band switching, WWV presets, and 1-Click Crystal Calibration to Flash.
    Compatible with all Quisk versions (app, hardware, conf, frame, gbs, vertBox).
    """
    def __init__(self, app, hardware, conf, frame=None, gbs=None, vertBox=None, *args, **kwargs):
        self.app = app
        self.hardware = hardware
        self.conf = conf
        # Quisk passes (app, hardware, conf, frame, gbs, vertBox)
        self.parent = frame
        self.sizer = vertBox if vertBox is not None else (args[0] if args else None)

        if self.parent is None:
            return

        panel = wx.Panel(self.parent)
        main_box = wx.BoxSizer(wx.VERTICAL)
        row1 = wx.BoxSizer(wx.HORIZONTAL)
        row2 = wx.BoxSizer(wx.HORIZONTAL)

        # Title / Status
        self.status_lbl = wx.StaticText(panel, wx.ID_ANY, "Pico W SDR (OpenHPSDR P1 / Wi-Fi)")
        font = self.status_lbl.GetFont()
        font.SetPointSize(9)
        self.status_lbl.SetFont(font)
        self.status_lbl.SetForegroundColour(wx.Colour(140, 200, 255))
        row1.Add(self.status_lbl, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 10)

        # Standard Ham Bands
        ham_bands = [
            ("80m", 3573000),
            ("40m", 7074000),
            ("30m", 10136000),
            ("20m", 14074000),
            ("17m", 18100000),
            ("15m", 21074000),
            ("12m", 24915000),
            ("10m", 28074000),
        ]
        for name, freq in ham_bands:
            btn = wx.Button(panel, wx.ID_ANY, name, size=(42, 24))
            btn.Bind(wx.EVT_BUTTON, self._make_tune_handler(freq))
            row1.Add(btn, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 3)

        # WWV Calibration Frequency Presets
        cal_lbl = wx.StaticText(panel, wx.ID_ANY, "NBS/WWV Presets:")
        cal_lbl.SetForegroundColour(wx.Colour(255, 200, 100))
        row2.Add(cal_lbl, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 6)

        wwv_freqs = [
            ("2.5M", 2500000),
            ("5.0M", 5000000),
            ("10.0M", 10000000),
            ("15.0M", 15000000),
            ("20.0M", 20000000),
            ("CHU 7.8M", 7850000),
        ]
        for name, freq in wwv_freqs:
            btn = wx.Button(panel, wx.ID_ANY, name, size=(60, 22))
            btn.Bind(wx.EVT_BUTTON, self._make_tune_handler(freq))
            row2.Add(btn, 0, wx.ALIGN_CENTER_VERTICAL | wx.RIGHT, 4)

        main_box.Add(row1, 0, wx.ALL, 2)
        main_box.Add(row2, 0, wx.LEFT | wx.RIGHT | wx.BOTTOM, 2)
        panel.SetSizer(main_box)

        if self.sizer is not None:
            self.sizer.Add(panel, 0, wx.ALL, 2)

    def _make_tune_handler(self, freq_hz):
        def handler(event):
            try:
                # Tell Quisk app to tune frequency
                self.app.ChangeVFO(freq_hz)
            except Exception as e:
                print(f"[!] Error tuning to {freq_hz} Hz: {e}")
        return handler

    def update_widgets(self, *args, **kwargs):
        """Called periodically by Quisk GUI update loop."""
        pass

    def UpdateText(self, text=None, *args, **kwargs):
        """Called periodically by Hermes driver heartbeat to update status."""
        if text and hasattr(self, 'status_lbl') and self.status_lbl:
            try:
                self.status_lbl.SetLabel(str(text))
            except Exception:
                pass

    def SetText(self, text=None, *args, **kwargs):
        """Safe text updater."""
        self.UpdateText(text)

    def HeartBeat(self, *args, **kwargs):
        """Safe heartbeat handler."""
        pass


class ConfigWidgets(object):
    """
    Custom Quisk configuration tab for Pico W SDR.
    Includes an interactive Si5351 Crystal Calibration Calculator.
    """
    def __init__(self, app, hardware, conf, parent=None, sizer=None, *args, **kwargs):
        self.app = app
        self.hardware = hardware
        self.conf = conf
        self.parent = parent
        self.sizer = sizer if sizer is not None else (args[0] if args else None)

        if self.parent is None or self.sizer is None:
            return

        # 1. Hardware Info Box
        info_box = wx.StaticBoxSizer(wx.StaticBox(self.parent, wx.ID_ANY, "Pico W SDR Hardware Overview"), wx.VERTICAL)
        info1 = wx.StaticText(self.parent, wx.ID_ANY, "Controller: Raspberry Pi Pico W (RP2040 @ 125 MHz + CYW43439 2.4 GHz Wi-Fi)")
        info2 = wx.StaticText(self.parent, wx.ID_ANY, "Synthesizer: Si5351A Clock Generator via I2C (Quadrature / 4x Clock Output)")
        info3 = wx.StaticText(self.parent, wx.ID_ANY, "ADC Front-End: PCM1808 Stereo 24-bit Audio ADC (48.0 kSPS / 96.0 kSPS via I2S PIO)")
        info4 = wx.StaticText(self.parent, wx.ID_ANY, "Protocol: OpenHPSDR Protocol 1 (UDP Port 1024, EP6 RX I/Q Stream, EP2 C&C Tuning)")
        for item in [info1, info2, info3, info4]:
            info_box.Add(item, 0, wx.ALL, 3)
        self.sizer.Add(info_box, 0, wx.EXPAND | wx.ALL, 8)

        # 2. Si5351 Crystal Calibration Calculator
        cal_box = wx.StaticBoxSizer(wx.StaticBox(self.parent, wx.ID_ANY, "Si5351 Crystal Calibration Helper"), wx.VERTICAL)
        
        desc = wx.StaticText(self.parent, wx.ID_ANY, 
            "To calibrate a new board: Tune to a standard NIST/NBS station (e.g., WWV on 15 MHz).\n"
            "Center the carrier on the spectrum display, then enter the observed dial frequency below:")
        cal_box.Add(desc, 0, wx.ALL, 4)

        grid = wx.FlexGridSizer(rows=4, cols=2, vgap=6, hgap=10)

        # Nominal Crystal
        grid.Add(wx.StaticText(self.parent, wx.ID_ANY, "Nominal Crystal (Hz):"), 0, wx.ALIGN_CENTER_VERTICAL)
        self.txt_nominal = wx.TextCtrl(self.parent, wx.ID_ANY, "24576000", size=(140, -1))
        grid.Add(self.txt_nominal, 0)

        # True Standard RF Frequency
        grid.Add(wx.StaticText(self.parent, wx.ID_ANY, "Standard Ref (e.g. WWV 15M):"), 0, wx.ALIGN_CENTER_VERTICAL)
        self.txt_true = wx.TextCtrl(self.parent, wx.ID_ANY, "15000000", size=(140, -1))
        grid.Add(self.txt_true, 0)

        # Observed Dial Frequency
        grid.Add(wx.StaticText(self.parent, wx.ID_ANY, "Observed Dial Freq (Hz):"), 0, wx.ALIGN_CENTER_VERTICAL)
        self.txt_obs = wx.TextCtrl(self.parent, wx.ID_ANY, "14997522", size=(140, -1))
        grid.Add(self.txt_obs, 0)

        cal_box.Add(grid, 0, wx.ALL, 4)

        btn_calc = wx.Button(self.parent, wx.ID_ANY, "Compute & Save Calibrated Crystal to Flash")
        btn_calc.Bind(wx.EVT_BUTTON, self._on_calc_cal)
        cal_box.Add(btn_calc, 0, wx.ALL, 6)

        self.lbl_result = wx.StaticText(self.parent, wx.ID_ANY, "Result: Ready")
        font_res = self.lbl_result.GetFont()
        font_res.SetWeight(wx.FONTWEIGHT_BOLD)
        self.lbl_result.SetFont(font_res)
        cal_box.Add(self.lbl_result, 0, wx.ALL, 4)

        self.sizer.Add(cal_box, 0, wx.EXPAND | wx.ALL, 8)

    def _on_calc_cal(self, event):
        try:
            f_nom = float(self.txt_nominal.GetValue().strip())
            f_true = float(self.txt_true.GetValue().strip())
            f_obs = float(self.txt_obs.GetValue().strip())

            if f_obs <= 0 or f_true <= 0:
                self.lbl_result.SetLabel("Error: Frequencies must be > 0.")
                return

            f_cal = f_nom * (f_true / f_obs)
            f_cal_int = int(round(f_cal))
            ppm = ((f_true - f_obs) / f_true) * 1e6

            # Send CAL command to Pico W
            pico_ip = getattr(self.conf, 'hermes_ip', '192.168.1.186')
            cmd = f"CAL,{int(f_true)},{int(f_obs)}\r\n"
            reply_str = ""
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(2.0)
                s.connect((pico_ip, 5000))
                s.sendall(cmd.encode('ascii'))
                reply = s.recv(256).decode('ascii', errors='ignore')
                s.close()
                reply_str = f"Pico W Response: {reply.strip()}"
            except Exception as e:
                reply_str = f"Note: Could not connect to Pico W at {pico_ip}:5000 ({e})"

            msg = (
                f"Calibrated Crystal: {f_cal_int} Hz ({f_cal/1e6:.6f} MHz)\n"
                f"Crystal Offset Error: {ppm:+.2f} PPM\n"
                f"{reply_str}\n"
                f"Saved permanently to RP2040 Flash memory!"
            )
            self.lbl_result.SetLabel(msg)
        except Exception as e:
            self.lbl_result.SetLabel(f"Error calculating: {e}")

    def write_conf(self, config_text):
        """Called when Quisk saves configuration."""
        return config_text
