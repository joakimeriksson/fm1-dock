# Licensing

Dinghy is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
`LICENSE`). You may use, study, change and share it. If you give someone a firmware built from it, give them its
complete source under the same licence too.

## Who made what

| | |
| --- | --- |
| The platform: `firmware/hal/`, `firmware/loader/`, `crt0.S`, `app.ld`, `firmware/src/` (`usb.c`, `ota.c`, `recovery.c`, `lcd.c`, `gfx.c`, `libc.c`, `bootguard.h`, `uac_fir.h`), `tools/` | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with the changes of [SLOOP](https://github.com/isod89/sloop-fm1) (isod89) |
| The app `firmware/src/dinghy.c`, the wiring `platform.c`, the host mode `tests/`, the docs | zvenson |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Terminus font 8x16 (ter-u16n), rendered into `firmware/gen/felucca_font.h` | SIL OFL 1.1 | `assets/fonts/Terminus-LICENSE.txt` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

`firmware/src/midi_uart.c` and `firmware/hal/fm1_uart.h` (TRS MIDI IN) are Copyright (C) 2026 Leo Kuroshita, Hügelton Instruments, GPL-3.0, taken from Felucca; `tests/midi_uart_test.c` is a host test for it.

## setBfree

The organ model in `firmware/src/dinghy.c` (wheel gear ratios, drawbar footage mapping, percussion and scanner
parameters, MIDI controller numbers) is derived from setBfree, <https://github.com/pantherb/setBfree>,
Copyright (C) Fredrik Kilander, Will Panther, Robin Gareus, Ken Restivo and others, GPL-2.0-or-later. The code here
is a fixed-point rewrite for the FM-1, not a copy of setBfree's source; the combined firmware is GPL-3.0-only.
`tools/gen_organ.py` and the generated `firmware/src/organ_tab.h` carry the wheel tables. `data/setbfree-default.pgm` is a
copy of setBfree's `pgm/default.pgm` (GPL-2.0-or-later); `tools/gen_programs.py` turns it into `firmware/src/organ_pgm.h`.
`data/setbfree-contrib.txt` is setBfree's own key/drawbar/wheel gain matrix (dumped with `tools/dump_contrib.c`, GPL-2.0-or-later); `tools/gen_contrib.py` turns it into `firmware/src/organ_contrib.h`.
`data/setbfree-whirl.txt` is setBfree's whirl (Leslie) table dump (`tools/dump_whirl.c`, GPL-2.0-or-later); `tools/gen_leslie.py` turns it into `firmware/src/organ_leslie.h`. The reverb in `organ_fx.h` follows `b_reverb/reverb.c`.
The controller numbers and value ranges follow setBfree's `src/midi.c`.

## The logo

`assets/logo.svg` / `logo.png` carry the designburg signet: Copyright (C) designburg, all rights reserved, not under
the GPL. It marks Dinghy itself; a fork uses its own logo.

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments; "M-VAVE" and "FM-1" belong to their
owners. Dinghy is not made or endorsed by them.
