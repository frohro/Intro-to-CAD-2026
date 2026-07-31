# Restoring Research firmwares

The C/UAC1 firmwares under the old untracked `Research/` directory were **not**
in git. During reorg they were moved to:

`../Intro-to-CAD-2026-WIP-aside/Research`

That aside directory is **no longer present** on this machine (removed outside
the workspace). If you have another copy (Time Machine, external drive, zip
emails, another PC), restore as follows.

## v0.2

```bash
SRC=/path/to/Research
DST=v0.2/Software/SDR/firmware
for d in 2026_v0.2 2026_v0.2_Multi 48_kHz_2026_v0.2 96_kHz_2026_v0.2 \
         48_kHz_CJC_5340_2026_v0.2 96_kHz_CJC_5340_2026_v0.2 192_kHz_CJC_5340_2026_v0.2
do
  rsync -a --exclude 'build/' --exclude 'build_*/' --exclude '__pycache__/' \
    "$SRC/$d/" "$DST/$d/"
done
```

## v0.1

```bash
rsync -a --exclude 'build/' --exclude 'build_*/' --exclude '__pycache__/' \
  /path/to/Research/96_kHz_2026/ v0.1/Software/SDR/firmware/96_kHz_2026/
```

## Docs that lived in Research

- `2026-Overall-Plan.md` → place in `Docs/`
- Optional plot: `Ideal-vs-non-ideal_op-amp9.png` → `Docs/`

Optional experimental trees **not** imported by plan: `WiFi/`, `96_kHz_SDR-TRX/`.
