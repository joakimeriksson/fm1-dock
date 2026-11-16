# Licensing

ORRERY (this repository, orrery-fm1) is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
`LICENSE`). You may use, study, change and share it. If you give someone a module built from it, give them its
complete source under the same licence too.

## Who made what

| | |
| --- | --- |
| The module `module/`, the portable session `core/`, the instruments' adapters and the effects box `instruments/`, the effects `effects/`, the tools and the tests | Joakim Eriksson |
| The ARP 2600 with its 1601 sequencer `engines/arp_*`, the Eminent 310 with the Small Stone `engines/em_*`, the Korg Mini Pops 7 `engines/mp_*` | Joakim Eriksson, written for [fm1-dock](https://github.com/joakimeriksson/fm1-dock); copied in by `take.py` |
| The Elka Synthex `engines/sc_*` | Joakim Eriksson ([Scintilla](https://github.com/joakimeriksson/scintilla-fm1)), by way of fm1-dock |
| The ENSEMBLE pads `engines/ens_engine.c`, `ens_engine.h`, `ens_tables.h` (`tools/gen_ens_tables.py`) | ported from [Caravel](https://github.com/bonszaimusic/caravel) 0.11.0's ENSEMBLE by bonszaimusic (GPL-3.0-only), with the DSP blocks it uses from [Felucca-Engines](https://github.com/hugelton/Felucca-Engines) by Leo Kuroshita (@kurogedelic), Hügelton Instruments (MIT) |
| The reverb `effects/reverb.c` and the tape echo `effects/echo.c` | Joakim Eriksson: fm1-dock's echo as an instance a session owns; the reverb began as Scintilla's (by way of fm1-dock) and keeps its lines and laws, with a half-rate loop, moving taps and input allpasses of its own |
| The clock's arithmetic `core/transport.c` | Joakim Eriksson, following the remainder carry of `int_clock_units` in [Melodee](https://github.com/keremimo/melodee) (Kerem Kilic, GPL-3.0); no function of Melodee's is copied |

The module is built against fm1-dock's module interface (`firmware/dock/hb_module.h` in its tree, GPL-3.0-only);
that file is not in this one.

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Felucca-Engines' DSP blocks (`src/dsp.c`: the fixed-point helpers, the sine, the polyBLEP) in `engines/ens_engine.c` | MIT, Copyright (c) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments | `LICENSES/MIT-Felucca-Engines.txt` |

## Names

ARP 2600, Eminent 310, Korg Mini Pops 7, Electro-Harmonix Small Stone and Elka Synthex are the names of the
instruments these engines are modelled on, and belong to their owners; so do "M-VAVE", "FM-1", "Felucca" and
"Hügelton Instruments". Nothing here is made or endorsed by them, and no recording or sample of anyone's is in it:
every sound is synthesized.
