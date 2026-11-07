# Niagara Drift licensing

Niagara Drift is free software under the GNU General Public License, version 3 only
(`GPL-3.0-only`, full text in `LICENSE`). It starts from [FoMni](https://github.com/charlesvestal/fm1-fomni), which is built on Felucca's
platform layer (through X0X), and keeps their licence. If you distribute Niagara Drift, or firmware derived from it, you must give
your recipients its complete corresponding source under the same licence.

## Where the code comes from

| What | Origin | Licence |
| --- | --- | --- |
| Platform: `firmware/hal/`, `firmware/loader/`, `firmware/src/{libc,lcd,gfx,usb,storage,ota,midi_uart}.c`, `tools/` (build, package, install, rescue), `web/fm1*.js` | [Felucca](https://github.com/hugelton/Felucca), Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments, as changed for [X0X](https://github.com/charlesvestal/fm1-x0x), FoMni and Niagara Drift | GPL-3.0-only |
| `firmware/src/app/{panel,plat_fm1,main_fm1}.c`, `host/omni_host.c`, `web/emu/` | adapted from X0X (itself from Felucca's `panel.c`, `main.c`, `audio.c`) | GPL-3.0-only |
| Key pressure: `firmware/src/app/sense.c`, `tests/host/sense_test.c`, the analog row sampling and LED glow in `firmware/hal/fm1_input.h` and `fm1_adc.h`, the pressure tracking in `firmware/src/app/plat_fm1.c` | [MONK-1](https://github.com/charlesvestal/fm1-monk) by Charles Vestal (commit 7937106), from [Velo](https://github.com/charlesvestal/fm1-velocity)'s sensing | GPL-3.0-only |
| The delay's and reverb's design: `firmware/src/dsp/drone_fx.c` (the stereo delay's types and its wow, after the Moog Matriarch's; the eight-line feedback delay network with its Hadamard mix, diffusers, half-rate half-band filters and their coefficients, line lengths and Inf), written anew in float | [ChoralRoot](https://github.com/Quixotic7/ChoralRootFM1) by Quixotic7 (`firmware/src/cr_fx.c`, `docs/FX.md`; commit 9fc83fa) | GPL-3.0-only |
| The update's rule for another writer and its boot area (any well-made writer; a boot area the FM-1 boots, so M-VAVE's V15 and V16 go back over each other's bootloader): `ota_boot_valid()` and steps 2–3 of `ota_stage()` in `firmware/src/ota.c`, and their checks in `tests/ota_test.c` | [felucca-bsp](https://github.com/hugelton/felucca-bsp) by Charles Vestal (`chip/ota.c`, commit afd83ed; the test after [fm1-base](https://github.com/charlesvestal/fm1-base)'s) | MPL-2.0 (`LICENSES/MPL-2.0.txt`; MPL-2.0 joins a GPL work, §3.3, the files not marked otherwise) |
| The idea of a browser emulator for FM-1 firmware (`web/emu/`) | [Groove OS](https://www.groove-os.com/) by Peter Gombos, whose site runs its firmware in the browser; no code from it | credit only |
| Everything else in `firmware/src/{app,dsp}`, `tests/`, unless its header says otherwise | [FoMni](https://github.com/charlesvestal/fm1-fomni) by Charles Vestal | GPL-3.0-only |
| New files added by Niagara Drift (marked in their headers) | Niagara Drift | GPL-3.0-only |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Barlow Semi Condensed (The Barlow Project Authors), the UI face | SIL OFL 1.1 | `assets/fonts/BarlowSemiCondensed-*.ttf`, `assets/fonts/Barlow-OFL.txt` |
| Inter Tight (The Inter Project Authors), IBM Plex Mono (IBM Corp., Reserved Font Name "Plex") and Barlow Semi Condensed, the emulator page's faces, as served by Google Fonts (latin subset, unmodified) | SIL OFL 1.1 | `web/emu/fonts/*.woff2`, with `InterTight-OFL.txt`, `IBMPlexMono-OFL.txt`, `Barlow-OFL.txt` beside them |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## Trademarks

Omnichord is a trademark of Suzuki Musical Instrument Corporation, named here only because FoMni,
whose code is still in this tree, was inspired by it. "Felucca" and "Hügelton Instruments" are names of Hügelton Instruments.
"M-VAVE" and "FM-1" are trademarks of their respective owners. Niagara Drift is independent firmware; it is
not affiliated with, endorsed by or supported by any of them.

## Radio

Niagara Drift never enables the Bluetooth / Wi-Fi radio of the hardware.
