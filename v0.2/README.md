# Board v0.2

## Hardware

Open [`Hardware/Intro-to-CAD-2026.kicad_pro`](Hardware/Intro-to-CAD-2026.kicad_pro).

Includes dual-ADC options (PCM1808 preferred; CJC5340 retained). Instructor
`Hardware/Frohne/` libs remain because the merged schematic references them.

## Software

| Path | Role |
|------|------|
| `Software/SDR/MicroPythonSDR/` | Pico MicroPython + Quisk helpers |
| `Software/SDR/Soapy-For-2026-Board/` | Soapy module |
| `Software/SDR/firmware/` | C UAC1 firmwares (PCM1808, CJC, Multi) — restore if empty |

Preferred firmware entry point once restored: `firmware/2026_v0.2/` (PCM1808 48/96).
CJC and Multi trees are kept for research even if PCM1808 is preferred on the bench.
