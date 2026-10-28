# Licensing

fm1-dock is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
`LICENSE`). You may use, study, change and share it. If you give someone a firmware built from it, give them its
complete source under the same licence too.

## Who made what

| | |
| --- | --- |
| The platform: `firmware/hal/`, `firmware/loader/`, `crt0.S`, `app.ld`, `firmware/src/` (`usb.c`, `ota.c`, `recovery.c`, `lcd.c`, `gfx.c`, `libc.c`, `bootguard.h`, `uac_fir.h`), `tools/` | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with the changes of [SLOOP](https://github.com/isod89/sloop-fm1) (isod89) |
| The host `firmware/src/dock.c`, the module interface `firmware/dock/`, the tools for modules, deploys and probes (`tools/dock.py`, `hbmod.py`, `hbdeploy.py`, `hbboot.py`, `probe.py`), `probes/`, the page `docs/` | Joakim Eriksson |
| The keys' force sensing `firmware/src/sense.c` | Velo's, by way of CS-1 (twochannels) |
| The modules: CS-1 `modules/cs1/` | twochannels ([CS-1](https://github.com/twochannels/CS-1)), wrapped by Joakim Eriksson |
| Scintilla `modules/scintilla/` | Joakim Eriksson ([Scintilla](https://github.com/joakimeriksson/synthex-fm1)) |
| OberVave `modules/obervave/` | zvenson ([OberVave](https://github.com/zvenson/obervave)), wrapped by Joakim Eriksson |
| The second core: `firmware/hal/fm1_cpu1.h`, `fm1_cpu1.S` | Charles Vestal ([X0X](https://github.com/charlesvestal/fm1-x0x)), after Melodee's dual-core audio by Kerem Kilic ([Melodee](https://github.com/keremimo/melodee)), by way of SLOOP's dualcore branch (Andre Rezemini) |
| Its use in the host: `firmware/src/cpu1.c` | after SLOOP's `cpu1.c` (Andre Rezemini); the screening and the checks: Joakim Eriksson |
| FM-1 SID `modules/sid/` (with reSID 0.16 by Dag Lem, GPL-2.0-or-later) | Joakim Eriksson |
| The base: Dinghy's app `firmware/src/dinghy.c`, the wiring `platform.c`, the host mode `tests/`, `docs/API.md`, `RULES.md`, `MAKE-IT-YOURS.md` | zvenson ([Dinghy](https://github.com/zvenson/dinghy)) |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Terminus font 8x16 (ter-u16n), rendered into `firmware/gen/felucca_font.h` | SIL OFL 1.1 | `assets/fonts/Terminus-LICENSE.txt` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## The logo

`assets/logo.svg` / `logo.png` carry the designburg signet: Copyright (C) designburg, all rights reserved, not under
the GPL. It marks Dinghy itself; a fork uses its own logo.

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments; "M-VAVE" and "FM-1" belong to their
owners. Dinghy is not made or endorsed by them.
