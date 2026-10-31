#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""modules/monk/monk/ from MONK-1's own tree (github.com/charlesvestal/fm1-monk): its app (app/: the project, the UI,
the face, the inflater, its platform interface plat.h), its voice (dsp/: MonkSynth's, MIT and GPL-3.0) and its drawing
(gfx.c), with the two headers its build makes: the fonts (felucca_font.h, its tools/gen_font.py: Pillow with libraqm)
and the characters (art.h, its tools/gen_art.py from assets/characters/: Pillow). The platform under it is fm1-dock's:
monk_platform.inc. Run it again on a new MONK-1:
    modules/monk/take.py ~/src/fm1-monk [FONT_H]        (MONK-1 a9d368a)
FONT_H: gen_font.py's output made elsewhere when this Python's Pillow has no libraqm (see modules/amb/take.py).

MONK-1 on its own firmware has 432 KiB of RAM (96 KiB and a 336 KiB pool), a module 272 KiB: what changes is where
things are kept, not what they hold, so the voice is sample for sample MONK-1's and the screen pixel for pixel:
  - no .pool section (a module's RAM is one region, cleared by the host);
  - the voice's overlap buffer 1152 floats instead of 2048: a grain lands at most a block (256) ahead of the read
    and is 882 long, so 1138 are ever in use; the index wraps by % instead of a mask (36 KiB for ten voices);
  - the screen's canvas 240 x 80 instead of 240 x 144 (the menu's strip is the tallest drawn at once; the scene at a
    new singer goes in three bands of 80 rows instead of two of 120), and the mouth's coverage and clip buffers in
    the canvas after the region they draw over (the face uses both at once only while it draws the mouth);
  - the mouth rasterizer's tables to what the twelve characters use, measured over every pose (24 vowels, 5
    loudnesses, eyes open and closed): 776 edges and 75 points at most, the largest region 12971 pixels."""
import re
import shutil
import subprocess
import sys
from pathlib import Path

src = Path(sys.argv[1])
fw = src / "firmware/src"
out = Path(__file__).resolve().parent / "monk"
for d in ["app", "dsp"]:
    (out / d).mkdir(parents=True, exist_ok=True)
for f in ["app/app.h", "app/plat.h", "app/project.c", "app/ui.c", "app/face.c", "app/inflate.c", "dsp/monk.c",
          "dsp/monk.h", "dsp/fastmath.h", "gfx.c"]:
    shutil.copy(fw / f, out / f)
for f, to in [("LICENSE", "LICENSE"), ("README.md", "README.md"), ("assets/fonts/Barlow-OFL.txt", "Barlow-OFL.txt")]:
    shutil.copy(src / f, out / to)
if len(sys.argv) > 2:
    shutil.copy(sys.argv[2], out / "felucca_font.h")
else:
    subprocess.run([sys.executable, str(src / "tools/gen_font.py"), str(out / "felucca_font.h")], check=True)
subprocess.run([sys.executable, str(src / "tools/gen_art.py"), str(out / "art.h")], check=True)


def sub(name, old, new):
    t = (out / name).read_text()
    assert t.count(old) == 1, (name, old, t.count(old))
    (out / name).write_text(t.replace(old, new))


POOL = '__attribute__((section(".pool")))'
sub("app/plat.h", "#define OM_POOL " + POOL, "#define OM_POOL                        /* fm1-dock: one RAM region (take.py) */")
sub("dsp/monk.c", "#define MK_POOL " + POOL, "#define MK_POOL                        /* fm1-dock: one RAM region (take.py) */")

# the overlap buffer: 1138 in use (a block ahead of the read, and a grain)
sub("dsp/monk.c", "#define OLAP 2048u                    /* > a block (256) + a grain */",
    "#define OLAP 1152u                    /* > a block (256) + a grain; fm1-dock: not a power of 2 (take.py) */")
t = (out / "dsp/monk.c").read_text()
assert t.count("& (OLAP - 1u)") == 2
(out / "dsp/monk.c").write_text(t.replace("& (OLAP - 1u)", "% OLAP"))

# the mouth's tables, to the characters' needs (the largest region from art.h, the rest as measured)
regions = [int(w) * int(h) for w, h in re.findall(r"\}, \d+, \d+, \d+, \d+, \d+, \d+, (\d+), (\d+),\n",
                                                  (out / "art.h").read_text())]
assert len(regions) == 12 and max(regions) <= 12992, regions
sub("app/face.c", "#define RS_PT 1024                     /* source path points */",
    "#define RS_PT 128                      /* source path points (fm1-dock: 75 used, take.py) */")
sub("app/face.c", "#define RS_EDGE 1536                   /* polygon edges for one fill */",
    "#define RS_EDGE 1024                   /* polygon edges for one fill (fm1-dock: 776 used, take.py) */")
sub("app/face.c", "#define RS_COV 16384                   /* coverage / clip buffers: the largest region, gen_art checks */",
    "#define RS_COV 12992                   /* coverage / clip buffers: the largest region, gen_art checks (fm1-dock: "
    "12971, take.py) */")
sub("app/face.c", "static uint8_t rs_cov[RS_COV] OM_POOL, rs_clip[RS_COV] OM_POOL;",
    "#define rs_cov ((uint8_t *)cv_px + 2u * RS_COV)   /* fm1-dock: in the canvas, after the region (take.py) */\n"
    "#define rs_clip (rs_cov + RS_COV)")
sub("gfx.c", """#define CV_MAX (240u * 144u)      /* X0X: the main area, both bands, is 240 x 144 */
#ifdef OM_HOST                   /* MONK-1: the host build (tests/host/ui_sim.c) */
static uint16_t cv_px[CV_MAX];
#else
static uint16_t cv_px[CV_MAX] __attribute__((section(".pool")));
#endif""", """#define CV_MAX (2u * 12992u)      /* fm1-dock: the menu's 240 x 80, or the face's region and its coverage and
                                 * clip (face.c RS_COV), one RAM region (take.py) */
static uint16_t cv_px[CV_MAX];""")
sub("app/ui.c", """        scene_rows(0, 120);
        cv_blit(0, 0);
        scene_rows(120, 120);
        cv_blit(0, 120);""", """        scene_rows(0, 80);              /* fm1-dock: in three bands, the canvas's (take.py) */
        cv_blit(0, 0);
        scene_rows(80, 80);
        cv_blit(0, 80);
        scene_rows(160, 80);
        cv_blit(0, 160);""")
for f in out.rglob("*.[ch]"):
    assert POOL not in f.read_text(), f
print("monk/: taken from", src)
