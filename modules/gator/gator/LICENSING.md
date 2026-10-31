# GATOR licensing

GATOR is free software, licensed under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
`LICENSE`). That covers the firmware, the tools, the web installer, the tests and GATOR's own assets. A few bundled
parts keep their own licences; they are listed below, and their licence texts are in `LICENSES/`.

Copyright (C) 2026 npi3pak (GATOR: the app, the web installer page, the release tooling and the logo)

GATOR is built on the Felucca platform, Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
(GPL-3.0-only), through SLOOP and ORBIT. The files taken from it keep their copyright headers.

You may use, study, change and share GATOR under the GPL. If you distribute GATOR, or firmware derived from it, you
must also give your recipients its complete corresponding source under the same licence.

GATOR is provided without any warranty (GPL-3.0 sections 15 and 16). Installing it is at your own risk.

**Source code.** The complete corresponding source of every released package is the tag `vX.Y` of
<https://github.com/npi3pak/fm1-gator> (GitHub attaches its archives to the release); the web installer links the
source of the version it installs. `BUILDING.md` describes how to build it.

**Modified files.** The platform files taken from ORBIT 0.6.0 (dspaudio/orbit) and changed for GATOR keep their
copyright line and carry a second one, "Modified for GATOR by npi3pak, 2026, from ORBIT 0.6.0". Files without that
line are unchanged from ORBIT (the HAL and the update loader's code, whose binary is byte-identical to ORBIT's),
or, with only an npi3pak line, new in GATOR.

## What is GPL-3.0-only

Every file in this tree that carries an `SPDX-License-Identifier: GPL-3.0-only` header, and GATOR's assets:

- the firmware: `firmware/` (the GATOR app and the demo app, the system layer, the HAL, the update loader)
- the build scripts and tools: `build.sh`, `tools/` (not `tools/sdk/`, below)
- the web installer and its tests: `web/`
- the host simulator and tests: `host/`
- the GATOR logo: `firmware/apps/gator/logo.png`, `web/logo.jpg`, `docs/img/gator.jpg`

## Platform and third-party material

| What | Licence | Where |
| --- | --- | --- |
| Felucca by Leo Kuroshita, Hügelton Instruments (<https://github.com/hugelton/Felucca>): the HAL, the update loader, USB-MIDI, OTA, recovery, flash storage, the `.fwsc` packer and the command-line installer | GPL-3.0-only | `firmware/hal/`, `firmware/loader/`, `firmware/sys/`, `tools/` |
| ORBIT by dspaudio (<https://github.com/dspaudio/orbit>), a fork of SLOOP: the platform GATOR was cut down from | GPL-3.0-only | as above |
| SLOOP by isod89 (<https://github.com/isod89/sloop-fm1>), a fork of Felucca: the web install protocol (`fm1ota.js`, `fm1pkg.js`, unchanged from SLOOP 2.7), the stock supply levels at start | GPL-3.0-only | `web/fm1ota.js`, `web/fm1pkg.js`, `firmware/hal/fm1_sys.h` |
| ChoralRoot by Quixotic7 (<https://github.com/Quixotic7/ChoralRootFM1>): the order of the audio interrupt's acknowledge (0.18); the release and Pages workflows follow its layout | GPL-3.0-only | `firmware/sys/audio.c`, `.github/workflows/` |
| Plaits and stmlib by Emilie Gillet (<https://github.com/pichenettes/eurorack>, <https://github.com/pichenettes/stmlib>): the synthesis DSP, with the changes listed in `third_party/eurorack/PATCHES.md` | MIT | `third_party/eurorack/`, `LICENSES/MIT-Plaits-stmlib.txt` |
| Terminus font by Dimitar Toshkov Zhekov: the UI text, rasterised into the firmware at build time (`tools/gen_font.py`) | SIL OFL 1.1 | `assets/fonts/ter-u16n.bdf`, `LICENSES/OFL-Terminus.txt` |
| JieLi AC79 SDK by JieLi Technology: three of its files go into every `.fwsc` package (below) | Apache-2.0 | `tools/sdk/`, `LICENSES/Apache-2.0-JieLi-SDK.txt` |

GATOR is inspired by Mutable Instruments Plaits and names it as the origin of its DSP. Following the author's
guidelines for derivative works, it does not carry the "Mutable Instruments" name or the module's name in its own
name or on its screen, and it is not affiliated with or endorsed by Mutable Instruments.

## JieLi SDK files in the packages (Apache-2.0)

A `.fwsc` package made by `tools/build.py` holds three unmodified files from the JieLi AC79 SDK, tag
`AC79NN_SDK_V1.2.1_2023-12-13` (<https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK>, `cpu/wl82/tools/`), vendored in
`tools/sdk/` with the SDK's licence:

| File in the package | What it is |
| --- | --- |
| `uboot.boot` | the first-stage boot loader (SPL) |
| `cfg_tool.bin` | the chip configuration block |
| `eq_cfg_hw.bin` | the default hardware EQ table |

They are licensed under the Apache License, Version 2.0, not under the GPL, and their copyright stays with JieLi
Technology. The SDK has no NOTICE file. Every distribution of a package carries the licence texts with it: the
release (`tools/release.py`) and the site (`web/make_site.py`, next to the packages in `firmware/`).

## No vendor material

No M-VAVE material is part of GATOR. The firmware links no vendor code and contains no vendor data. The web
installer's "Back to the official firmware" holds only the official file's size and SHA-256: you select the file you
downloaded from M-VAVE yourself, and it is checked and installed in your browser, never uploaded or redistributed.

## Trademarks

"M-VAVE" and "FM-1" are trademarks of their respective owners. GATOR is independent firmware that runs on FM-1
hardware; it is not affiliated with, endorsed by or supported by them. "Mutable Instruments" and "Plaits" are
names of their owner; GATOR is not affiliated with Mutable Instruments.

The name GATOR and its logo identify this project. Forks are welcome under the GPL; please give a modified firmware
another name and logo so that users can tell it apart.

## Contributions

Contributions are welcome under GPL-3.0-only.
