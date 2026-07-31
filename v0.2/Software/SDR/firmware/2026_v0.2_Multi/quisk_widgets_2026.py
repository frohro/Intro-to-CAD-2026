from __future__ import print_function
from __future__ import absolute_import
from __future__ import division

import wx


class BottomWidgets:
    def __init__(self, app, hardware, conf, frame, gbs, vertBox):
        self.hardware = hardware
        self.application = app
        self.frame = frame
        self.num_rows_added = 1
        start_row = app.widget_row
        start_col = app.button_start_col

        # Create dropdown
        self.choice = wx.Choice(frame, choices=[])
        self.choice.Bind(wx.EVT_CHOICE, self.OnChoice)
        gbs.Add(self.choice, (start_row, start_col), (1, 5), flag=wx.EXPAND)

        # Create ADC dropdown
        self.adc_choice = wx.Choice(frame, choices=["PCM1808", "CJC5430"])
        self.adc_choice.Bind(wx.EVT_CHOICE, self.OnADCChoice)
        gbs.Add(self.adc_choice, (start_row, start_col + 5), (1, 2), flag=wx.EXPAND)

        # Status text height helper
        dummy = app.QuiskCheckbutton(frame, None, text="")
        _, bh = dummy.GetMinSize()
        dummy.Destroy()

        self.status = app.QuiskText(frame, hardware._widget_summary(), bh)
        gbs.Add(self.status, (start_row, start_col + 7), (1, 13), flag=wx.EXPAND)

        # Link hardware to this class so it can trigger UI refreshes
        self.hardware.application.bottom_widgets = self
        self.UpdateChoices()

    def _candidate_key(self, candidate):
        if not candidate:
            return None
        return (
            int(candidate.get("lo", 0)),
            int(candidate.get("n", 0)),
            int(candidate.get("a", 0)),
            int(candidate.get("b", 0)),
            int(candidate.get("c", 0)),
        )

    def _current_choice_type(self):
        active = getattr(self.hardware, '_active_candidate', None)
        pair = getattr(self.hardware, '_comparison_pair', None)
        active_key = self._candidate_key(active)

        if active_key and pair:
            for choice_type in ("best", "middle", "worst", "fract"):
                if self._candidate_key(pair.get(choice_type)) == active_key:
                    return choice_type

        if active and int(active.get("b", 0)) != 0:
            return "fract"

        if getattr(self.hardware, '_comparison_use_fract', False):
            return "fract"
        if getattr(self.hardware, '_comparison_use_worst', False):
            return "worst"
        if getattr(self.hardware, '_comparison_use_middle', False):
            return "middle"
        return "best"

    def UpdateChoices(self):
        """Refresh the dropdown list based on available integer LOs."""
        current_type = self._current_choice_type()

        new_choices = self.hardware.get_lo_choices()
        
        # Only update if the labels have changed to avoid flickering
        new_labels = [c[0] for c in new_choices]
        old_labels = [c[0] for c in self.choices_data] if hasattr(self, 'choices_data') else []
        
        if new_labels != old_labels:
            self.choices_data = new_choices
            self.choice.Clear()
            for label in new_labels:
                self.choice.Append(label)

        # Restore selection based on state
        for i, (_, data) in enumerate(self.choices_data):
            if data["type"] == current_type:
                self.choice.SetSelection(i)
                break
        else:
            self.choice.SetSelection(0)

    def OnADCChoice(self, event):
        idx = self.adc_choice.GetSelection()
        label = self.adc_choice.GetString(idx)
        self.hardware.set_adc(label)
        self.UpdateStatus()

    def OnChoice(self, event):
        idx = self.choice.GetSelection()
        label, data = self.choices_data[idx]

        if data["type"] == "best":
            self.hardware.set_comparison_mode()
        elif data["type"] == "middle":
            self.hardware.set_comparison_mode(use_middle=True)
        elif data["type"] == "worst":
            self.hardware.set_comparison_mode(use_worst=True)
        elif data["type"] == "fract":
            self.hardware.set_comparison_mode(use_fract=True)
        elif data["type"] == "custom":
            self.PromptCustomLO()
            return  # PromptCustomLO calls UpdateStatus itself

        self.UpdateStatus()

    def PromptCustomLO(self):
        dlg = wx.TextEntryDialog(
            self.frame,
            "Enter custom LO frequency in Hz (e.g. 7050000):",
            "Custom LO Entry",
        )
        if dlg.ShowModal() == wx.ID_OK:
            try:
                freq = float(dlg.GetValue().replace(",", "").strip())
                self.hardware.set_custom_lo(freq)
            except ValueError:
                wx.MessageBox("Invalid frequency. Please enter a number in Hz.", "Error")
        dlg.Destroy()
        # Always reset away from "Custom LO..." after the dialog so that
        # selecting it again will fire EVT_CHOICE even if it was already selected.
        self.UpdateStatus()

    def UpdateStatus(self):
        self.UpdateChoices()
        self.status.SetLabel(self.hardware._widget_summary())
        # Synchronize the ADC choice dropdown with actual hardware state
        adc_name = getattr(self.hardware, "_adc_name", "PCM1808")
        idx = self.adc_choice.FindString(adc_name)
        if idx != wx.NOT_FOUND:
            self.adc_choice.SetSelection(idx)