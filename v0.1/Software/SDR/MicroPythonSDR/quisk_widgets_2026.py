from __future__ import print_function
from __future__ import absolute_import
from __future__ import division

import wx


class BottomWidgets:
    def __init__(self, app, hardware, conf, frame, gbs, vertBox):
        self.hardware = hardware
        self.application = app
        self.num_rows_added = 1
        start_row = app.widget_row
        start_col = app.button_start_col

        self.btn_worst = app.QuiskCheckbutton(frame, self.OnBtnWorst, text='Worst Int')
        self.btn_worst.SetValue(getattr(hardware, '_comparison_use_worst', False))
        gbs.Add(self.btn_worst, (start_row, start_col), (1, 2), flag=wx.EXPAND)

        bw, bh = self.btn_worst.GetMinSize()
        self.status = app.QuiskText(frame, hardware._widget_summary(), bh)
        gbs.Add(self.status, (start_row, start_col + 2), (1, 18), flag=wx.EXPAND)

    def OnBtnWorst(self, event):
        self.hardware.set_comparison_mode(event.GetEventObject().GetValue())
        self.UpdateStatus()

    def UpdateStatus(self):
        self.status.SetLabel(self.hardware._widget_summary())