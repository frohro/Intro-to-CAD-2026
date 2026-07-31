# Intro-to-CAD-2026

QSD SDR receiver board (WWU Intro to CAD) with versioned hardware and software.

## Layout

```text
v0.1/                  # First board revision
  Hardware/            # KiCad project for v0.1
  Software/SDR/        # MicroPython, Soapy, firmware/
v0.2/                  # Second board revision (CJC5340 option, etc.)
  Hardware/            # KiCad project for v0.2
  Software/SDR/        # MicroPython, Soapy, firmware/
Docs/                  # Plans, credits, restore notes
```

Pick the **root folder that matches the PCB silkscreen / board revision** you have.

| Board | Open KiCad | Software |
|-------|------------|----------|
| v0.1 | [v0.1/Hardware/Intro-to-CAD-2026.kicad_pro](v0.1/Hardware/Intro-to-CAD-2026.kicad_pro) | [v0.1/Software/SDR](v0.1/Software/SDR) |
| v0.2 | [v0.2/Hardware/Intro-to-CAD-2026.kicad_pro](v0.2/Hardware/Intro-to-CAD-2026.kicad_pro) | [v0.2/Software/SDR](v0.2/Software/SDR) |

Duplication between `v0.1` and `v0.2` is intentional so each board is self-contained.

## Firmware status

C firmwares from the old `Research/` tree still need to be restored into
`*/Software/SDR/firmware/` if you have a backup. See [Docs/FIRMWARE_RESTORE.md](Docs/FIRMWARE_RESTORE.md).

## History

- Tag `archive/pre-version-split` — layout before this reorg (student `Module_*` at repo root).
- Long-lived git branch `v0.2` held hardware work before the monorepo split; prefer paths under `v0.2/` on `master` going forward.
- Student module credits: [Docs/CREDITS.md](Docs/CREDITS.md).
