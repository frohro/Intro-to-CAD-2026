# v0.2 SDR firmware

Expected firmware trees (from the former `Research/` folder):

| Folder | Description |
|--------|-------------|
| `2026_v0.2/` | PCM1808 48/96 kHz dual-rate UAC1 (preferred starting point) |
| `48_kHz_2026_v0.2/` | PCM1808 dual-rate variant |
| `96_kHz_2026_v0.2/` | PCM1808 96 kHz |
| `48_kHz_CJC_5340_2026_v0.2/` | CJC5340 48 kHz (kept for research) |
| `96_kHz_CJC_5340_2026_v0.2/` | CJC5340 96 kHz |
| `192_kHz_CJC_5340_2026_v0.2/` | CJC5340 192 kHz (16-bit USB) |
| `2026_v0.2_Multi/` | Multi-ADC experimental (PCM1808 + CJC5340) |

**Restore note:** These sources were untracked and lived only under `Research/`.
During the reorg they were moved aside and that aside copy is no longer on disk.
If you still have the Research trees (backup, another machine, zip), copy them
here **without** `build/` directories:

```bash
rsync -a --exclude 'build/' --exclude 'build_*/' --exclude '__pycache__/' \
  /path/to/Research/2026_v0.2/ v0.2/Software/SDR/firmware/2026_v0.2/
```

PCM1808 is the preferred ADC path; CJC5340 and Multi are retained for future work.
