#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""modules/dub/dub/ from DUB-1's own tree (github.com/sbegueria/fm1-dub, a fork of AMB-1 on FoMni's platform): its app
(app/: the project, the pieces' recipes, the UI, its platform interface plat.h), its engine (dsp/: the chord stabs, the
pings, the bass, the drums, the noise machine, the tape echo, the cloud and the plate) and its drawing (gfx.c)
unchanged, and its fonts (felucca_font.h, made by DUB-1's tools/gen_font.py: Pillow with libraqm). One change for a
module's RAM: the .pool section (the firmware's second RAM region) is gone, a module's RAM is one region the host
clears; two more make the engine fit it (below). The platform under it is fm1-dock's: dub_platform.inc. Run it again
on a new DUB-1:
    modules/dub/take.py ~/src/fm1-dub [FONT_H]        (DUB-1 501c4cf)
FONT_H: gen_font.py's output made elsewhere when this Python's Pillow has no libraqm (see modules/amb/take.py)."""
import shutil
import subprocess
import sys
from pathlib import Path

src = Path(sys.argv[1])
fw = src / "firmware/src"
out = Path(__file__).resolve().parent / "dub"
for d in ["app", "dsp"]:
    (out / d).mkdir(parents=True, exist_ok=True)
for f in ["app/app.h", "app/plat.h", "app/project.c", "app/recipe_prints.h", "app/ui.c", "dsp/amb.c", "dsp/amb.h",
          "dsp/fastmath.h", "gfx.c"]:
    shutil.copy(fw / f, out / f)
for f, to in [("LICENSE", "LICENSE"), ("LICENSING.md", "LICENSING.md"),
              ("assets/fonts/Barlow-OFL.txt", "Barlow-OFL.txt"), ("assets/fonts/Terminus-LICENSE.txt", "Terminus-LICENSE.txt")]:
    shutil.copy(src / f, out / to)
if len(sys.argv) > 2:
    shutil.copy(sys.argv[2], out / "felucca_font.h")
else:
    subprocess.run([sys.executable, str(src / "tools/gen_font.py"), str(out / "felucca_font.h")], check=True)


def sub(name, old, new):
    t = (out / name).read_text()
    assert t.count(old) == 1, (name, old, t.count(old))
    (out / name).write_text(t.replace(old, new))


POOL = '__attribute__((section(".pool")))'
sub("app/plat.h", "#define EN_POOL " + POOL, "#define EN_POOL                        /* fm1-dock: one RAM region (take.py) */")
sub("dsp/amb.c", "#define EN_POOL " + POOL, "#define EN_POOL                        /* fm1-dock: one RAM region (take.py) */")
sub("gfx.c", "static uint16_t cv_px[CV_MAX] " + POOL + ";", "static uint16_t cv_px[CV_MAX];   /* fm1-dock: one RAM region (take.py) */")
# two things for a module's 272 KiB of RAM (DUB-1 on its own firmware has 340), as modules/amb/take.py makes them for
# AMB-1, whose code they are: the tape loop's length TAPE_N (its echo times already keep to it: the longest loop
# shorter), and the plate's delay lines in 16 bits (PL_Q: a float's 4 bytes to 2, as the tape keeps its samples)
sub("dsp/amb.c", "#define TAPE_N 62000                             /* 5.6 s at 11.025 kHz */",
    "#ifndef TAPE_N                                   /* fm1-dock: settable (take.py) */\n"
    "#define TAPE_N 62000                             /* 5.6 s at 11.025 kHz */\n#endif")
sub("dsp/amb.c", """static float pl_buf[DL_TOTAL] EN_POOL;
typedef struct {
    float *b;
    int n, i;
} dl_t;""", """#ifdef PL_Q                                       /* fm1-dock: the delay lines in 16 bits, PL_Q a unit (take.py) */
typedef int16_t pl_t;
static inline float pl_ld(pl_t v) { return (float)v * (1.0f / (float)PL_Q); }
static inline pl_t pl_st(float x)
{
    x *= (float)PL_Q;
    return (pl_t)(x > 32767.0f ? 32767.0f : x < -32767.0f ? -32767.0f : x + (x < 0.0f ? -0.5f : 0.5f));
}
#else
typedef float pl_t;
#define pl_ld(v) (v)
#define pl_st(x) (x)
#endif
static pl_t pl_buf[DL_TOTAL] EN_POOL;
typedef struct {
    pl_t *b;
    int n, i;
} dl_t;""")
sub("dsp/amb.c", """    return d->b[j < 0 ? j + d->n : j];
}""", """    return pl_ld(d->b[j < 0 ? j + d->n : j]);
}""")
sub("dsp/amb.c", """    d->b[d->i] = x;
    if (++d->i >= d->n)""", """    d->b[d->i] = pl_st(x);
    if (++d->i >= d->n)""")
sub("dsp/amb.c", """    float *p = pl_buf;
    int k, i;""", """    pl_t *p = pl_buf;
    int k, i;""")
sub("dsp/amb.c", """        pl_buf[i] = 0.0f;""", """        pl_buf[i] = 0;""")
for f in out.rglob("*.[ch]"):
    assert POOL not in f.read_text(), f
print("dub/: taken from", src)
