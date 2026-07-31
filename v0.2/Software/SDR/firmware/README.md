# v0.2 SDR firmware

Sources imported from `cptr-480-2026-instructor/Research` (build trees excluded).

| Folder | Description |
|--------|-------------|
| `2026_v0.2/` | PCM1808 48/96 kHz dual-rate UAC1 (**preferred starting point**) |
| `48_kHz_2026_v0.2/` | PCM1808 dual-rate / Johnson-mode variant |
| `96_kHz_2026_v0.2/` | PCM1808 96 kHz |
| `48_kHz_CJC_5340_2026_v0.2/` | CJC5340 48 kHz (research; PCM1808 preferred) |
| `96_kHz_CJC_5340_2026_v0.2/` | CJC5340 96 kHz |
| `192_kHz_CJC_5340_2026_v0.2/` | CJC5340 192 kHz (16-bit USB + dither) |
| `2026_v0.2_Multi/` | Multi-ADC experimental (PCM1808 + CJC5340) |

Build each project with the Pico SDK / CMake flow described in that folder's README.
Do not commit `build/` output.
