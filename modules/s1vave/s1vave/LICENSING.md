# Licensing

**S1Vave** is free software distributed under the **GNU General Public License, version 3 only** (`GPL-3.0-only`, full text in [LICENSE](LICENSE)). You are free to use, study, modify, and redistribute it. If you distribute firmware packages built from this source, you must make the complete source code available under the same license terms.

**S1Vave** is an open-source derivative work based on **OberVave** by **zvenson** ([Dinghy platform](https://github.com/zvenson/dinghy)), re-architected and transformed into an 8-voice virtual analog tweak synth with S-1 sound engine parity.

---

## Authors & Contributors

| Component | Authors & Copyright Holders |
|:---|:---|
| **S-1 Parity Sound Engine, DSP Calibration & Visual Branding**<br>`s1vave.c`, `obv_params.h`, `obv_dsp.h`, `obv_engine.h`, `obv_seq.h`, `obv_fx.h`, `obv_ui.h`, `splash_s1vave.h`, acoustic calibrations, USB audio watermarks, volume gain staging, S1VAVE vector typography and cyber visual identity | Copyright (C) 2026 **Andrew GG** ([AndrewGG88](https://github.com/AndrewGG88)) (GPL-3.0-only) |
| **OberVave Ancestor, Platform Wiring & Dinghy HAL**<br>Original OberVave synthesizer foundation, `platform.c`, host test harness `tests/`, build pipeline (`build.sh`, `tools/build.py`) | Copyright (C) 2026 **zvenson** ([OberVave](https://github.com/zvenson/obervave) / [Dinghy](https://github.com/zvenson/dinghy)) |
| **Hardware Abstraction Layer & Base Architecture**<br>`firmware/hal/`, `firmware/loader/`, `crt0.S`, `app.ld`, `usb.c`, `ota.c`, `recovery.c`, `lcd.c`, `gfx.c`, `bootguard.h` | Copyright (C) 2026 **Leo Kuroshita** (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with enhancements by **isod89** ([SLOOP](https://github.com/isod89/sloop-fm1)) |
| **M-VAVE SMK-37 Board Layer**<br>`smk_input.h`, `fm1_flash_map.h`, SMK-37 support across HAL, loader, and installer tools | Copyright (C) 2026 **M. Geisler** (genkigenki/makkuro), from [sloop-fm1 smk37](https://github.com/genkigenki/sloop-fm1/tree/smk37) |
| **Web Browser Simulator**<br>`web/sim/` (`sim.c`, AudioWorklet engine) | Copyright (C) 2026 zvenson, following the AudioWorklet simulator patterns of [Makkuro37](https://github.com/genkigenki/Makkuro37) and [winampFM](https://github.com/hericdk/winampFM) (both GPL-3.0) |

---

## Third-Party Components & Fonts

| Component | License | Origin / Reference |
|:---|:---|:---|
| **Terminus Font 8x16** (`felucca_font.h`) | SIL Open Font License 1.1 | `assets/fonts/Terminus-LICENSE.txt` |
| **Andrew GG Logo & Brand Identity** (`assets/logo.*`, `splash.png`, icons) | Proprietary / All Rights Reserved (see [TRADEMARK.md](TRADEMARK.md)) | Copyright (C) 2026 **Andrew GG**. Common-law trademark (TM). Exclusive property of Andrew GG. |
| **Trapezoidal SVF Filter Topology** | Public Domain / Academic | Andrew Simper (Cytomic) 24 dB/oct state-variable filter model |
| **Plate Reverb Tank** | Public Domain / Academic | Jon Dattorro (1997) figure-of-eight recirculating network |
| **JieLi AC79 SDK Binaries** | Apache-2.0 | `uboot.boot`, `cfg_tool.bin` read from SDK during build. No proprietary SDK source code is distributed in this repository. |

---

## Trademarks & Disclaimers

- **Roland**, **AIRA**, **AIRA Compact**, **S-1**, and **SH-101** are registered trademarks of **Roland Corporation**.
- **M-VAVE**, **FM-1**, and **SMK-37** are trademarks of Shenzhen Sincoo Technology Co., Ltd.
- **S1Vave** is an independent, community-driven open-source project. It is **not** manufactured, affiliated with, sponsored, or endorsed by Roland Corporation or Shenzhen Sincoo Technology Co., Ltd.
- No trademarked product names or logos appear in the binary runtime firmware.
