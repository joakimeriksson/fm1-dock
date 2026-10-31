#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""modules/gator/gator/ from GATOR's own tree (github.com/npi3pak/fm1-gator): its app (app.c: the pages, the presets,
the panel), its voice (macro.cc: the modulation matrix, the envelope, the LFOs around Plaits' Voice), Emilie Gillet's
Plaits and stmlib (MIT, third_party/eurorack, with GATOR's PATCHES.md), and of its platform (Felucca by way of ORBIT)
the app's interface (fw.h, fw_rate.h), the drawing (gfx.c), libc.c's string helpers and the C++ shims (cxx/), all
unchanged. The font (felucca_font.h) is made by GATOR's tools/gen_font.py (Pillow) from Terminus, its FONT_S cut to
ASCII 32..126 (12 KiB less; the app writes nothing else, the rest shows as '?'). The platform under it is fm1-dock's:
gator_platform.inc; the .pool section goes with GATOR's own FW_HOST switch (module.cfg). Run it again on a new GATOR:
    modules/gator/take.py ~/src/fm1-gator [FONT_H]        (GATOR 0.6, 92f923e)
FONT_H: gen_font.py's output made elsewhere, when this Python has no Pillow."""
import re
import shutil
import subprocess
import sys
from pathlib import Path

src = Path(sys.argv[1])
fw = src / "firmware"
out = Path(__file__).resolve().parent / "gator"
if out.exists():
    shutil.rmtree(out)
(out / "cxx").mkdir(parents=True)
for f in ["apps/gator/app.c", "apps/gator/macro.cc", "apps/gator/macro.h", "sys/fw.h", "sys/fw_rate.h", "sys/gfx.c",
          "sys/libc.c"]:
    shutil.copy(fw / f, out / Path(f).name)
for f in ["new", "algorithm", "cmath", "runtime.cc"]:
    shutil.copy(fw / "cxx" / f, out / "cxx" / f)
shutil.copytree(src / "third_party/eurorack", out / "eurorack")
for f, to in [("LICENSE", "LICENSE"), ("LICENSING.md", "LICENSING.md"), ("README.md", "README.md"),
              ("LICENSES/MIT-Plaits-stmlib.txt", "MIT-Plaits-stmlib.txt"), ("LICENSES/OFL-Terminus.txt", "OFL-Terminus.txt")]:
    shutil.copy(src / f, out / to)
font = out / "felucca_font.h"
if len(sys.argv) > 2:
    shutil.copy(sys.argv[2], font)
else:
    subprocess.run([sys.executable, "-I", str(src / "tools/gen_font.py"), str(font)], check=True)

# FONT_S to ASCII 32..126: its first 95 glyphs (FONT_L, 32..95, draws FONT_S's bitmaps at 2x)
t = font.read_text()
N = 127 - 32
m = re.search(r"static const uint16_t FONT_S_OFF\[(\d+)\] = \{([^}]*)\};", t)
assert m and int(m.group(1)) == 224, "FONT_S is not 32..255"
offs = [int(x) for x in m.group(2).split(",")]
cut = offs[N]
t = t.replace(m.group(0), f"static const uint16_t FONT_S_OFF[{N}] = {{" + ", ".join(map(str, offs[:N])) + "};")
for arr in ["ADV", "BW"]:
    m = re.search(rf"static const uint8_t FONT_S_{arr}\[224\] = \{{([^}}]*)\}};", t)
    assert m, arr
    t = t.replace(m.group(0), f"static const uint8_t FONT_S_{arr}[{N}] = {{" + ", ".join(m.group(1).split(", ")[:N]) + "};")
m = re.search(r"static const uint8_t FONT_S_DATA\[(\d+)\] = \{\n(.*?)\n\};", t, re.S)
assert m, "FONT_S_DATA"
data = [x for x in m.group(2).replace("\n", " ").replace(",", " ").split()]
assert len(data) == int(m.group(1))
rows = [data[i:i + 24] for i in range(0, cut, 24)]
rows[-1] = rows[-1][:cut - 24 * (len(rows) - 1)]
t = t.replace(m.group(0), f"static const uint8_t FONT_S_DATA[{cut}] = {{\n" +
              "\n".join("    " + ", ".join(r) + "," for r in rows) + "\n};")
old = "static const felucca_font_t FONT_S = { 16, 2, 32, 255, 0,"
assert t.count(old) == 1, "FONT_S"
t = t.replace(old, "static const felucca_font_t FONT_S = { 16, 2, 32, 126, 0,")
first = t.index("\n")
t = t[:first] + "\n/* fm1-dock: FONT_S cut to ASCII 32..126 by modules/gator/take.py, the rest shows as '?' */" + t[first:]
font.write_text(t)

POOL = '__attribute__((section(".pool")))'
for f in out.rglob("*.[ch]*"):              # (only as fw.h's FW_POOL and macro.cc's MACRO_POOL: empty with FW_HOST)
    s = f.read_text(errors="replace")
    assert POOL not in s or (f.name in ("fw.h", "macro.cc") and s.count(POOL) == 1 and "#ifdef FW_HOST" in s), f
print("gator/: taken from", src)
