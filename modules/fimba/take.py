#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""modules/fimba/fimba/ from FiMba-1's own tree (github.com/jadamsowers/fm1-fimba, a fork of FoMni's): its app (app/:
the project, the UI, its platform interface plat.h), its engine (dsp/: the tines, bodies, effects and patterns) and
its drawing (gfx.c) unchanged. Its fonts are FoMni's (the same gen_font.py and font set): felucca_font.h here only
includes modules/fomni/fomni/felucca_font.h. Changes for a module's RAM (272 KiB, one region; FiMba-1's engine alone
takes 266 KiB of it): the .pool section (the firmware's second RAM region) is gone, the host clears the module's RAM;
and the canvas is a strip of FIMBA_CV_ROWS rows (fimba_platform.inc draws each band strip by strip), not FiMba-1's
240 x 144. One more for the dock's keys: a key's sensed velocity scales its pluck (key_pluck). The platform under it
is fm1-dock's: fimba_platform.inc. Run it again on a new FiMba-1 (and set its VERSION as OM_VERSION there):
    modules/fimba/take.py ~/src/fm1-fimba        (FiMba-1 0485ed0, 0.6.1)"""
import shutil
import sys
from pathlib import Path

src = Path(sys.argv[1])
fw = src / "firmware/src"
out = Path(__file__).resolve().parent / "fimba"
for d in ["app", "dsp"]:
    (out / d).mkdir(parents=True, exist_ok=True)
for f in ["app/app.h", "app/plat.h", "app/project.c", "app/ui.c", "dsp/kalimba.c", "dsp/kalimba.h", "dsp/fastmath.h",
          "gfx.c"]:
    shutil.copy(fw / f, out / f)
for f, to in [("LICENSE", "LICENSE"), ("LICENSING.md", "LICENSING.md"),
              ("assets/fonts/Barlow-OFL.txt", "Barlow-OFL.txt"), ("assets/fonts/Terminus-LICENSE.txt", "Terminus-LICENSE.txt")]:
    shutil.copy(src / f, out / to)
(out / "felucca_font.h").write_text('/* fm1-dock: FoMni\'s fonts are FiMba-1\'s (modules/fimba/take.py) */\n'
                                     '#include "../../fomni/fomni/felucca_font.h"\n')


def sub(name, old, new):
    t = (out / name).read_text()
    assert t.count(old) == 1, (name, old, t.count(old))
    (out / name).write_text(t.replace(old, new))


POOL = '__attribute__((section(".pool")))'
sub("app/plat.h", "#define OM_POOL " + POOL, "#define OM_POOL                        /* fm1-dock: one RAM region (take.py) */")
sub("dsp/kalimba.c", "#define KM_POOL " + POOL, "#define KM_POOL                        /* fm1-dock: one RAM region (take.py) */")
sub("gfx.c", "#define CV_MAX (240u * 144u)      /* X0X: the main area, both bands, is 240 x 144 */\n"
    "static uint16_t cv_px[CV_MAX] " + POOL + ";",
    "#define CV_MAX (240u * FIMBA_CV_ROWS)   /* fm1-dock: a strip of the band (fimba_platform.inc, take.py) */\n"
    "static uint16_t cv_px[CV_MAX];")
for f in out.rglob("*.[ch]"):
    assert POOL not in f.read_text(), f
# the keys' force: a sensed key plucks as hard as it lands (FiMba-1's fixed 100 at a velocity of 100)
sub("app/ui.c", "    uint8_t i = ui.key_n[k];\n    km_pluck(cents, vel, pan, delay);",
    "    uint8_t i = ui.key_n[k];\n    vel = fimba_key_vel(k, vel);              /* fm1-dock: the key's velocity (take.py) */\n"
    "    km_pluck(cents, vel, pan, delay);")
print("fimba/: taken from", src)
