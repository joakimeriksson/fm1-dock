#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""modules/fm106/fm106/ from FM-106's own tree (github.com/PatttF/fm-106): its app, engine, presets and
effects unchanged, but for what a module's RAM and host need: its .pool arrays in .bss (a module's RAM is
one region, cleared by the host), and no app_main (the host calls fm106_app_init and fm106_app_frame, the
split FM-106 has for its web build). Its `#include "platform.c"` finds fm106/platform.c: fm1-dock's
(fm106_platform.inc copied in, see there). Run it again on a new FM-106:
    modules/fm106/take.py ~/src/fm-106        (FM-106 db59d58)"""
import shutil
import sys
from pathlib import Path

src = Path(sys.argv[1]) / "firmware"
out = Path(__file__).resolve().parent / "fm106"
out.mkdir(exist_ok=True)
for f in ["src/fm106_adsr.h", "src/fm106_chorus.h", "src/fm106_engine.h", "src/fm106_fx.h", "src/fm106_fx_tables.h",
          "src/fm106_osc.h", "src/fm106_presets.h", "src/fm106_seq.h", "src/fm106_sysex.h", "src/fm106_tables.h",
          "src/fm106_vcf.h", "src/gfx.c", "gen/felucca_font.h"]:
    shutil.copy(src / f, out / Path(f).name)
for f in ["LICENSE", "LICENSING.md", "README.md"]:
    shutil.copy(src.parent / f, out / f)
POOL = ' __attribute__((section(".pool")))'
for f in ["fm106_fx.h", "gfx.c"]:
    t = (out / f).read_text()
    assert POOL in t, f
    (out / f).write_text(t.replace(POOL, ""))

s = (src / "src/fm106.c").read_text()
main = '''#ifndef FM106_WEB
static void app_main(void)
{
    fm106_app_init();
    for (;;) {
        fm106_app_frame();
    }
}
#endif
'''
assert s.count(main) == 1 and s.endswith(main), s[-200:]
s = s[:-len(main)] + "/* fm1-dock: no app_main; the host calls fm106_app_init and fm106_app_frame (modules/fm106/take.py) */\n"
(out / "fm106.c").write_text("/* fm1-dock: FM-106's firmware/src/fm106.c, taken by modules/fm106/take.py: do not edit */\n" + s)
shutil.copy(Path(__file__).resolve().parent / "fm106_platform.inc", out / "platform.c")
print("fm106/: taken from", src.parent)
