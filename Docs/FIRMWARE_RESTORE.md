# SDR firmware sources

C/UAC1 firmwares live under:

- `v0.1/Software/SDR/firmware/`
- `v0.2/Software/SDR/firmware/`

They were imported from the Real Time Systems instructor tree:

`~/Classes/Real_Time_Systems/cptr-480-2026-instructor/Research`

If you need to re-sync later (exclude build artifacts):

```bash
SRC=~/Classes/Real_Time_Systems/cptr-480-2026-instructor/Research
DST_V02=v0.2/Software/SDR/firmware
for d in 2026_v0.2 2026_v0.2_Multi 48_kHz_2026_v0.2 96_kHz_2026_v0.2 \
         48_kHz_CJC_5340_2026_v0.2 96_kHz_CJC_5340_2026_v0.2 192_kHz_CJC_5340_2026_v0.2
do
  rsync -a --delete --exclude 'build/' --exclude 'build_*/' --exclude '__pycache__/' \
    "$SRC/$d/" "$DST_V02/$d/"
done
rsync -a --delete --exclude 'build/' --exclude 'build_*/' --exclude '__pycache__/' \
  "$SRC/96_kHz_2026/" v0.1/Software/SDR/firmware/96_kHz_2026/
```

Optional trees **not** imported: `WiFi/`, `96_kHz_SDR-TRX/`, empty `96_kHz/` stub.
