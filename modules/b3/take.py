#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""modules/b3/b3/ from B3 ORGAN's own tree (github.com/R-Jeb/setBfree-fm1): its organ (organ_*.h: the wheels,
setBfree's tapering, leakage, programs and controllers, the scanner, percussion, overdrive, reverb and Leslie) and its
app unchanged, but for one split: dinghy.c's app_main becomes app_init(), the body of its forever loop app_loop()
(a do { } while (0), so a `continue` is still "the next pass"), its loop-wide variables file-level statics; and for a
module's RAM: the delay lines' ORGAN_POOL and gfx.c's .pool in .bss (a module's RAM is one region, cleared by the
host). The battery in the corner goes: a module has no battery reading (the screen's corner stays black). Its
`#include "platform.c"` finds b3/platform.c: fm1-dock's (b3_platform.inc copied in, see there). Run it again on a new
B3 ORGAN:
    modules/b3/take.py ~/src/setBfree-fm1        (B3 ORGAN v0.2, 7014954)"""
import shutil
import sys
from pathlib import Path

src = Path(sys.argv[1]) / "firmware"
out = Path(__file__).resolve().parent / "b3"
out.mkdir(exist_ok=True)
for f in ["src/organ_contrib.h", "src/organ_core.h", "src/organ_fx.h", "src/organ_leslie.h", "src/organ_od.h",
          "src/organ_pgm.h", "src/organ_tab.h", "src/gfx.c", "gen/felucca_font.h"]:
    shutil.copy(src / f, out / Path(f).name)
for f in ["LICENSE", "LICENSING.md", "README_ORGAN.md"]:
    shutil.copy(src.parent / f, out / f)
POOL = ' __attribute__((section(".pool")))'
t = (out / "gfx.c").read_text()
assert t.count(POOL) == 1
(out / "gfx.c").write_text(t.replace(POOL, ""))

s = (src / "src/dinghy.c").read_text()


def sub(old, new):
    global s
    assert s.count(old) == 1, (old[:70], s.count(old))
    s = s.replace(old, new)


sub('#define ORGAN_POOL __attribute__((section(".pool")))   /* the reverb and Leslie delay lines: zeroed at boot */',
    '#define ORGAN_POOL                              /* fm1-dock: the delay lines in .bss, which the host clears */')
sub('''static void app_main(void)
{
    struct { uint8_t magic, db[NB], vtype, vup, perc, perc3, fast, soft, les, od, drv, pgm, rev; int8_t oct; } set;
    uint32_t prev = 0, k, i, sel = 2, t_change = 0, dirty = 0, t_load = 0, seed = 12345u, saved_key = 0, last_key = 0;
    uint32_t drawn_db[NB], drawn_sel = 99, status_key = ~0u, drawn_batt = ~0u;
    uint8_t held[27];
    int32_t oct = 0;
''', '''/* fm1-dock: app_main's loop-wide variables, for app_init and app_loop (modules/b3/take.py) */
static struct { uint8_t magic, db[NB], vtype, vup, perc, perc3, fast, soft, les, od, drv, pgm, rev; int8_t oct; } set;
static uint32_t prev = 0, k, i, sel = 2, t_change = 0, dirty = 0, t_load = 0, seed = 12345u, saved_key = 0, last_key = 0;
static uint32_t drawn_db[NB], drawn_sel = 99, status_key = ~0u;
static uint8_t held[27];
static int32_t oct = 0;
static void app_init(void)                      /* fm1-dock: app_main up to its loop */
{
''')
sub('''    draw_text_box(0, 4, 120, &FONT_S, "B3 ORGAN v" APP_VER, C_ACC, 0);

    for (;;) {
''', '''    draw_text_box(0, 4, 120, &FONT_S, "B3 ORGAN v" APP_VER, C_ACC, 0);
}

static void app_loop(void)                      /* fm1-dock: a pass of app_main's loop, from the host's draw() */
{
    do {
''')
sub('''        {
            uint32_t bs = battery_state();
            if (bs != drawn_batt) { drawn_batt = bs; draw_battery(bs); }
        }
''', '''        (void)draw_battery;                     /* fm1-dock: no battery reading in a module */
''')
assert s.rstrip().endswith("    }\n}"), s[-40:]
s = s.rstrip()[:-len("    }\n}")] + "    } while (0);\n}\n"
(out / "b3.c").write_text("/* fm1-dock: B3 ORGAN's firmware/src/dinghy.c, split by modules/b3/take.py: do not edit */\n" + s)
shutil.copy(Path(__file__).resolve().parent / "b3_platform.inc", out / "platform.c")
print("b3/: taken from", src.parent)
