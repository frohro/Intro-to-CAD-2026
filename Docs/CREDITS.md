# Credits — Intro to CAD 2026 board modules

The final board was assembled by merging student (and instructor) KiCad modules
from the Intro to CAD course. Those module projects are **historical**; the
merged design lives under `v0.1/Hardware` and `v0.2/Hardware`.

Full module trees are preserved in git history at tag `archive/pre-version-split`
(paths `Module_*` and `Frohne/` at the repository root on that tag).

| Module folder | Notes |
|---------------|--------|
| `Module_Andre` | Student module (Andre) |
| `Module_Andre_old` | Earlier revision of Andre's module |
| `Module_JaronGarbi` | Student module (Jaron Garbi) |
| `Module_Joshua` | Student module (Joshua) |
| `Module_KyleS` | Student module (Kyle S); SD-card footprints retained under `Hardware/libs/Sd-card` |
| `Module_Micah` | Student module (Micah) |
| `Module_Template` | Starter template for student modules |
| `Frohne` | Instructor module (Rob Frohne); library pieces remain under each version's `Hardware/Frohne/` because the merged board still references those symbols/footprints |

To restore a full historical module tree:

```bash
git checkout archive/pre-version-split -- Module_Andre
```
