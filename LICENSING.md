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
| Scintilla `modules/scintilla/` | Joakim Eriksson ([Scintilla](https://github.com/joakimeriksson/scintilla-fm1)) |
| OberVave `modules/obervave/` | zvenson ([OberVave](https://github.com/zvenson/obervave)), wrapped by Joakim Eriksson |
| FM-106 `modules/fm106/` | PatttF ([FM-106](https://github.com/PatttF/fm-106); its DSP after [ultramaster_kr106](https://github.com/kayrockscreenprinting/ultramaster_kr106)), wrapped by Joakim Eriksson |
| AMB-1 `modules/amb/` (MPL-2.0, file by file) and FoMni `modules/fomni/` | Charles Vestal ([AMB-1](https://github.com/charlesvestal/fm1-amb), [FoMni](https://github.com/charlesvestal/fm1-fomni)), wrapped by Joakim Eriksson |
| MONK-1 `modules/monk/` | Charles Vestal ([MONK-1](https://github.com/charlesvestal/fm1-monk); its voice Jonathan Taylor's MonkSynth, MIT), wrapped by Joakim Eriksson |
| FiMba-1 `modules/fimba/` | Adam Sowers ([FiMba-1](https://github.com/jadamsowers/fm1-fimba)), wrapped by Joakim Eriksson |
| GATOR `modules/gator/` | npi3pak ([GATOR](https://github.com/npi3pak/fm1-gator); Plaits and stmlib by Emilie Gillet, MIT, `modules/gator/gator/eurorack/`), wrapped by Joakim Eriksson |
| DUB-1 `modules/dub/` | sbegueria ([DUB-1](https://github.com/sbegueria/fm1-dub); after Charles Vestal's AMB-1), wrapped by Joakim Eriksson |
| The second core: `firmware/hal/fm1_cpu1.h`, `fm1_cpu1.S` | Charles Vestal ([X0X](https://github.com/charlesvestal/fm1-x0x)), after Melodee's dual-core audio by Kerem Kilic ([Melodee](https://github.com/keremimo/melodee)), by way of SLOOP's dualcore branch (Andre Rezemini) |
| BLE-MIDI: `firmware/src/ble/` (`ble_*`, `rf_share.h`) and `firmware/hal/fm1_ble.h`, `fm1_ble_rf.h`, `ble_rf_tables_v15.h` | w0ts ([Optimist](https://github.com/w0ts/optimist), commit 5b06939, GPL-3.0-only); `hx.c` / `hx.h` and the calibration's hooks: Joakim Eriksson, the hooks after Melodee (Kerem Kilic) |
| The supplies' start-up `firmware/hal/fm1_power.h`; the second core's hold (`fm1_core1_stop`, `fm1_sys.h`), its own stack limit and the paced wait (`fm1_cpu1.h`), DBG's bits by core (`fm1_irq.h`) | Kerem Kilic (Ellic Studio) ([Melodee](https://github.com/keremimo/melodee)); the hold with portions by Leo Kuroshita (@kurogedelic), Hügelton Instruments (Felucca 1.6.1); the supplies in the form of w0ts's `fm1_supply.h` ([Optimist](https://github.com/w0ts/optimist)); brought in by Joakim Eriksson |
| Its use in the host: `firmware/src/cpu1.c` | after SLOOP's `cpu1.c` (Andre Rezemini); the screening and the checks: Joakim Eriksson |
| FM-1 SID `modules/sid/` (with reSID 0.16 by Dag Lem, GPL-2.0-or-later) | Joakim Eriksson |
| The base: Dinghy's app `firmware/src/dinghy.c`, the wiring `platform.c`, the host mode `tests/`, `docs/API.md`, `RULES.md`, `MAKE-IT-YOURS.md` | zvenson ([Dinghy](https://github.com/zvenson/dinghy)) |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Terminus font 8x16 (ter-u16n), rendered into `firmware/gen/felucca_font.h` | SIL OFL 1.1 | `assets/fonts/Terminus-LICENSE.txt` |
| The radio's start-up calibration `firmware/hal/fm1_rf_cal.h`: JieLi's AC79 SDK calibration (`wl_rf_common.a`) in C by Charles Vestal, from [Melodee](https://github.com/keremimo/melodee) (as felucca-bsp's); its hooks in `fm1_ble_rf.h` / `fm1_ble.h` after Melodee's (Kerem Kilic, GPL-3.0) | Apache-2.0, Copyright (c) 2024-2026 Zhuhai Jieli Technology Co., Ltd., Copyright (C) 2026 Charles Vestal | `LICENSES/` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## The logo

`assets/logo.svg` / `logo.png` carry the designburg signet: Copyright (C) designburg, all rights reserved, not under
the GPL. It marks Dinghy itself; a fork uses its own logo.

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments; "M-VAVE" and "FM-1" belong to their
owners. Dinghy is not made or endorsed by them.
