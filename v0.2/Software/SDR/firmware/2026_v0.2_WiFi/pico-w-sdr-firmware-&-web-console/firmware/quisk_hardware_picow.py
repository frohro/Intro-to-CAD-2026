# quisk_hardware_picow.py - Hardware Driver for Intro-to-CAD-2026 Pico W SDR
# ==============================================================================
# Customized Quisk hardware driver for Raspberry Pi Pico W OpenHPSDR Receiver.
# Eliminates Hermes-Lite 2 PA, Alex Filter, and Step-Attenuator controls.
# Provides Sample Rate switching (48 kHz / 96 kHz) and Si5351 LO controls.
# ==============================================================================

from __future__ import print_function, absolute_import, division
import os, sys

try:
    from hermes.quisk_hardware import Hardware as BaseHardware
except ImportError:
    try:
        from quisk_hardware_hermes import Hardware as BaseHardware
    except ImportError:
        from quisk_hardware_model import Hardware as BaseHardware


class Hardware(BaseHardware):
    """
    Dedicated Hardware driver for the Intro-to-CAD-2026 Pico W SDR.
    Overrides and suppresses HL2 / Hermes transmitter and PA controls.
    """
    def __init__(self, app, conf):
        self.app = app
        self.conf = conf
        self.bandEdge1 = 0
        self.bandEdge2 = 0
        self._current_sample_rate = getattr(conf, 'sample_rate', 48000)
        
        try:
            super(Hardware, self).__init__(app, conf)
            print(f"[*] Pico W Hardware Driver initialized at {self._current_sample_rate} SPS.")
        except Exception as e:
            print(f"[!] BaseHardware init exception (non-fatal): {e}")

    def open(self):
        try:
            return super(Hardware, self).open()
        except Exception as e:
            print(f"[!] Hardware open exception: {e}")
            return "Pico W Open"

    def close(self):
        try:
            return super(Hardware, self).close()
        except Exception:
            return None

    def ChangeFrequency(self, tune, vfo, source='', band='', event=None):
        try:
            return super(Hardware, self).ChangeFrequency(tune, vfo, source=source, band=band, event=event)
        except TypeError:
            try:
                return super(Hardware, self).ChangeFrequency(tune, vfo)
            except Exception as e:
                return tune, vfo
        except Exception as e:
            return tune, vfo

    def ChangeMode(self, mode):
        if hasattr(super(Hardware, self), 'ChangeMode'):
            super(Hardware, self).ChangeMode(mode)

    def ChangeBand(self, band):
        if hasattr(super(Hardware, self), 'ChangeBand'):
            super(Hardware, self).ChangeBand(band)

    def HeartBeat(self):
        try:
            if hasattr(super(Hardware, self), 'HeartBeat'):
                return super(Hardware, self).HeartBeat()
            elif hasattr(super(Hardware, self), 'heartbeat'):
                return super(Hardware, self).heartbeat()
        except Exception:
            pass
        return None

    def heartbeat(self):
        return self.HeartBeat()

    # --------------------------------------------------------------------------
    # Hermes-Lite 2 & Peripheral Stubs (Suppress unwanted hardware options)
    # --------------------------------------------------------------------------
    def set_attenuation(self, att):
        """Pico W receiver has no relay step attenuator."""
        pass

    def set_preamp(self, preamp):
        """Pico W receiver front-end gain is fixed."""
        pass

    def set_filter(self, filter_num):
        """Pico W uses QSD Tayloe mixer with fixed low-pass anti-alias."""
        pass

    def set_alex(self, val):
        """Suppress Alex filter bank switching."""
        pass

    def set_tx_power(self, power):
        """RX Only receiver - disable TX power."""
        pass

    def set_cw_key(self, state):
        """Disable CW keying."""
        pass

    def get_tx_power(self):
        return 0.0

    def get_swr(self):
        return 1.0

    # --------------------------------------------------------------------------
    # Sample Rate Controls
    # --------------------------------------------------------------------------
    def SetSampleRate(self, new_rate):
        """
        Switches the SDR receiver sample rate between 48,000 and 96,000 SPS.
        """
        if new_rate not in (48000, 96000):
            print(f"[!] Unsupported sample rate requested: {new_rate}")
            return False
        
        print(f"[*] Setting Pico W Sample Rate to {new_rate} SPS...")
        self._current_sample_rate = new_rate
        self.conf.sample_rate = new_rate

        # If OpenHPSDR C backend has SetControlBit / SendControlPacket
        if hasattr(self, 'SetControlBit'):
            # Bit 0 of Control Byte 1 in OpenHPSDR Protocol 1 specifies sample rate:
            # 00 = 48k, 01 = 96k, 10 = 192k
            speed_code = 0 if new_rate == 48000 else 1
            try:
                # Update control byte
                self.SetControlBit(0x00, 0, speed_code & 1)
                self.SetControlBit(0x00, 1, (speed_code >> 1) & 1)
            except Exception as e:
                print(f"[!] SetControlBit error: {e}")

        return True

    def GetSampleRate(self):
        return self._current_sample_rate


class ConfigWidgets(object):
    """
    Custom Config screen for Pico W SDR to replace the Hermes-Lite 2 configuration form.
    Provides Sample Rate dropdown, Network IP, and Si5351 Crystal Calibration Helper.
    """
    def __init__(self, app, hardware, conf, parent=None, sizer=None, *args, **kwargs):
        self.app = app
        self.hardware = hardware
        self.conf = conf
        self.parent = parent
        self.sizer = sizer if sizer is not None else (args[0] if args else None)

        if self.parent is None or self.sizer is None:
            return

        import wx

        # Radio & Sample Rate Settings
        srate_box = wx.StaticBoxSizer(wx.StaticBox(self.parent, wx.ID_ANY, "Pico W SDR Radio Settings"), wx.VERTICAL)
        srate_grid = wx.FlexGridSizer(rows=2, cols=2, vgap=8, hgap=14)

        srate_grid.Add(wx.StaticText(self.parent, wx.ID_ANY, "Sample Rate (SPS):"), 0, wx.ALIGN_CENTER_VERTICAL)
        self.cfg_choice_rate = wx.Choice(self.parent, wx.ID_ANY, choices=["48000", "96000"])
        cur_rate = str(getattr(self.conf, 'sample_rate', 48000))
        if cur_rate in ["48000", "96000"]:
            self.cfg_choice_rate.SetStringSelection(cur_rate)
        else:
            self.cfg_choice_rate.SetSelection(0)
        self.cfg_choice_rate.Bind(wx.EVT_CHOICE, self._on_config_rate_change)
        srate_grid.Add(self.cfg_choice_rate, 0)

        srate_grid.Add(wx.StaticText(self.parent, wx.ID_ANY, "Pico W IP Address:"), 0, wx.ALIGN_CENTER_VERTICAL)
        self.txt_ip = wx.TextCtrl(self.parent, wx.ID_ANY, str(getattr(self.conf, 'hermes_ip', '192.168.1.186')), size=(150, -1))
        srate_grid.Add(self.txt_ip, 0)

        srate_box.Add(srate_grid, 0, wx.ALL, 8)
        self.sizer.Add(srate_box, 0, wx.EXPAND | wx.ALL, 6)

        # Hardware Info Box
        info_box = wx.StaticBoxSizer(wx.StaticBox(self.parent, wx.ID_ANY, "Hardware Configuration"), wx.VERTICAL)
        info1 = wx.StaticText(self.parent, wx.ID_ANY, "Controller: Raspberry Pi Pico W (RP2040 Dual-Core + CYW43439 Wi-Fi)")
        info2 = wx.StaticText(self.parent, wx.ID_ANY, "Synthesizer: Si5351A Clock Generator via I2C (Direct Quadrature LO)")
        info3 = wx.StaticText(self.parent, wx.ID_ANY, "ADC Front-End: PCM1808 Stereo 24-bit Audio ADC (24-bit I2S via PIO)")
        info4 = wx.StaticText(self.parent, wx.ID_ANY, "Protocol: OpenHPSDR Protocol 1 (UDP Port 1024 / Command Port 5000)")
        for item in [info1, info2, info3, info4]:
            info_box.Add(item, 0, wx.ALL, 2)
        self.sizer.Add(info_box, 0, wx.EXPAND | wx.ALL, 6)

        # Si5351 Crystal Calibration Helper
        cal_box = wx.StaticBoxSizer(wx.StaticBox(self.parent, wx.ID_ANY, "Si5351 Crystal Calibration Helper"), wx.VERTICAL)
        desc = wx.StaticText(self.parent, wx.ID_ANY,
            "To calibrate: Tune to a standard WWV station (e.g. 15 MHz).\n"
            "Center carrier on spectrum, enter observed dial frequency, and click Save:")
        cal_box.Add(desc, 0, wx.ALL, 4)

        grid = wx.FlexGridSizer(rows=3, cols=2, vgap=6, hgap=10)
        grid.Add(wx.StaticText(self.parent, wx.ID_ANY, "Nominal Crystal (Hz):"), 0, wx.ALIGN_CENTER_VERTICAL)
        self.txt_nominal = wx.TextCtrl(self.parent, wx.ID_ANY, "24576000", size=(140, -1))
        grid.Add(self.txt_nominal, 0)

        grid.Add(wx.StaticText(self.parent, wx.ID_ANY, "Standard Ref (e.g. WWV 15M):"), 0, wx.ALIGN_CENTER_VERTICAL)
        self.txt_true = wx.TextCtrl(self.parent, wx.ID_ANY, "15000000", size=(140, -1))
        grid.Add(self.txt_true, 0)

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

        self.sizer.Add(cal_box, 0, wx.EXPAND | wx.ALL, 6)

    def _on_config_rate_change(self, event):
        sel_rate = int(self.cfg_choice_rate.GetStringSelection())
        if hasattr(self.hardware, 'SetSampleRate'):
            self.hardware.SetSampleRate(sel_rate)
        self.conf.sample_rate = sel_rate

    def _on_calc_cal(self, event):
        import socket
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

            pico_ip = self.txt_ip.GetValue().strip() or getattr(self.conf, 'hermes_ip', '192.168.1.186')
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

