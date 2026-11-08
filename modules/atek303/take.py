#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""modules/atek303/atek303/ from ATEK303's own tree (github.com/animatek/FM1-Animatek, its atek303/ on fm1-base): its
app (app.c, app.h, the factory presets and the splash), its voice, pattern generator and effects (dsp/) unchanged,
and from fm1-base the platform interface (plat.h), its maths (fastmath.h), its drawing (gfx.c) and its fonts
(felucca_font.h, made by fm1-base's tools/gen_font.py: Pillow). The platform under it is fm1-dock's:
atek303_platform.inc. Run it again on a new ATEK303:
    modules/atek303/take.py ~/src/FM1-Animatek [FONT_H]        (FM1-Animatek 564b7a0: ATEK303 1.2, fm1-base e07ebca)
FONT_H: gen_font.py's output made elsewhere when this Python's Pillow has no libraqm (see modules/amb/take.py).

ATEK303 on fm1-base has 370 KiB of RAM (40 KiB and a 330 KiB pool), a module 272 KiB. What changes, all settable in
ATEK303's own sources and off in its own build (its package stays byte for byte the same), set in module.cfg:
  - no .pool section (a module's RAM is one region, cleared by the host);
  - the plate reverb's lines in 16 bits (FX_RV_Q: a float's 4 bytes to 2, as its delay already keeps them);
  - the screen's canvas 240 x 124 instead of 240 x 144 (APP_CANVAS_PX: the tallest band ATEK303 draws but its splash,
    which a module does not show)."""
import shutil
import subprocess
import sys
from pathlib import Path

src = Path(sys.argv[1])
app = src / "atek303/app"
base = src / "atek303/base"                   # (a link to fm1-base, checked out beside it: its clonar-terceros.sh)
out = Path(__file__).resolve().parent / "atek303"
(out / "dsp").mkdir(parents=True, exist_ok=True)
for f in ["app.c", "app.h", "factory_presets.h", "splash_img.h", "dsp/atek_voice.c", "dsp/atek_voice.h",
          "dsp/acid_pattern.c", "dsp/acid_pattern.h", "dsp/fx.c", "dsp/fx.h", "dsp/LICENSE-open303.txt"]:
    shutil.copy(app / f, out / f)
for f in ["include/plat.h", "include/fastmath.h", "device/gfx.c"]:
    shutil.copy(base / f, out / Path(f).name)
for f, to in [(src / "LICENSE", "LICENSE"), (base / "assets/fonts/Barlow-OFL.txt", "Barlow-OFL.txt"),
              (base / "assets/fonts/Terminus-LICENSE.txt", "Terminus-LICENSE.txt")]:
    shutil.copy(f, out / to)
if len(sys.argv) > 2:
    shutil.copy(sys.argv[2], out / "felucca_font.h")
else:
    subprocess.run([sys.executable, str(base / "tools/gen_font.py"), str(out / "felucca_font.h")], check=True)


def sub(name, old, new, count=1):
    t = (out / name).read_text()
    assert t.count(old) == count, (name, old, t.count(old))
    (out / name).write_text(t.replace(old, new))


POOL = '__attribute__((section(".pool")))'
sub("plat.h", "#define PLAT_POOL " + POOL, "#define PLAT_POOL                      /* fm1-dock: one RAM region (take.py) */")
sub("dsp/fx.c", "#define FX_POOL " + POOL, "#define FX_POOL                        /* fm1-dock: one RAM region (take.py) */")
sub("gfx.c", "static uint16_t cv_px[CV_MAX] " + POOL + ";", "static uint16_t cv_px[CV_MAX];   /* fm1-dock: one RAM region (take.py) */")
for f in list(out.glob("*.[ch]")) + list(out.glob("dsp/*.[ch]")):
    assert POOL not in f.read_text(), f
rev = subprocess.run(["git", "-C", str(src), "describe", "--tags", "--always"], capture_output=True, text=True).stdout.strip()
print("atek303/: taken from", src, rev)
