#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""modules/niagara/niagara/ from Niagara Drift's own tree (github.com/evanyerburgh/niagara-drift, Evan Yerburgh): its
app (app/: the project, the UI, its platform interface plat.h), its engine (dsp/: the two drones, their LFO, gestures,
delay and reverb) and its drawing (gfx.c) unchanged, but for what a module's 272 KiB of RAM needs:
  - the .pool sections (the firmware's second RAM region) are gone: a module's RAM is one region the host clears;
  - the delay's two lines are 16384 samples, not 32768 (371 ms, not 743): its longest time 450 ms -> 270 ms
    (TIME's scale is the same up to there: 70 ms at 0, 270 ms at 100), so the LFO's 25 % and the wow still fit;
  - the knob loop keeps 2048 events, not 4096 (its 5 minutes stay; a very busy one fills sooner).
The font is FoMni's (modules/fomni/fomni/felucca_font.h: the same face, Barlow Semi Condensed, a quarter smaller than
Niagara Drift's own; its tools/gen_font.py is not run here): the UI's layout is unchanged, its text a little smaller,
and the middle dot "·" shows as FoMni's backtick. The platform under it is fm1-dock's: niagara_platform.inc. Run it
again on a new Niagara Drift:
    modules/niagara/take.py ~/src/niagara-drift        (Niagara Drift 86f47d2, v0.2.1)"""
import shutil
import sys
from pathlib import Path

src = Path(sys.argv[1])
fw = src / "firmware/src"
here = Path(__file__).resolve().parent
out = here / "niagara"
for d in ["app", "dsp"]:
    (out / d).mkdir(parents=True, exist_ok=True)
for f in ["app/app.h", "app/plat.h", "app/project.c", "app/ui.c", "dsp/drone.c", "dsp/drone.h", "dsp/drone_fx.c",
          "dsp/fastmath.h", "gfx.c"]:
    shutil.copy(fw / f, out / f)
for f, to in [("LICENSE", "LICENSE"), ("LICENSING.md", "LICENSING.md"), ("assets/fonts/Barlow-OFL.txt", "Barlow-OFL.txt")]:
    shutil.copy(src / f, out / to)
shutil.copy(here.parent / "fomni/fomni/felucca_font.h", out / "felucca_font.h")


def sub(name, old, new):
    t = (out / name).read_text()
    assert t.count(old) == 1, (name, old, t.count(old))
    (out / name).write_text(t.replace(old, new))


POOL = '__attribute__((section(".pool")))'
ONE = "/* fm1-dock: one RAM region (take.py) */"
sub("app/plat.h", "#define OM_POOL " + POOL, "#define OM_POOL                        " + ONE)
sub("dsp/drone_fx.c", "#define DR_POOL " + POOL, "#define DR_POOL                        " + ONE)
sub("gfx.c", "static uint16_t cv_px[CV_MAX] " + POOL + ";", "static uint16_t cv_px[CV_MAX];   " + ONE)
sub("dsp/drone_fx.c", "#define DLY_N 32768u                     /* 743 ms: 450 ms + the LFO's 25 % + the wow + room */",
    "#define DLY_N 16384u                     /* fm1-dock (take.py): 371 ms, its longest time 270 ms (743, 450) */")
sub("dsp/drone_fx.c", "static float dly_ms(int v) { return 70.0f * fm_exp2f((float)v * 0.026845f); }   /* 70 * (450/70)^(v/100) */",
    "static float dly_ms(int v) { return fm_minf(70.0f * fm_exp2f((float)v * 0.026845f), 270.0f); }   /* fm1-dock (take.py): to 270 ms */")
sub("dsp/drone.c", "itoa_s((int)(70.0f * fm_exp2f((float)v * 0.026845f) + 0.5f), b);",
    "itoa_s((int)(fm_minf(70.0f * fm_exp2f((float)v * 0.026845f), 270.0f) + 0.5f), b);   /* fm1-dock (take.py) */")
sub("app/ui.c", "float ms = 70.0f * fm_exp2f((float)proj.par[P_DLY_TIME] * 0.026845f);",
    "float ms = fm_minf(70.0f * fm_exp2f((float)proj.par[P_DLY_TIME] * 0.026845f), 270.0f);   /* fm1-dock (take.py) */")
sub("app/ui.c", "#define LOOP_MAX 4096 ", "#define LOOP_MAX 2048 /* fm1-dock (take.py): 4096 */ ")
for f in out.rglob("*.[ch]"):
    assert POOL not in f.read_text(), f
print("niagara/: taken from", src)
