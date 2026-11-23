#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""modules/flat20/flat20/ from FLAT-20's own tree (github.com/thenewwazoo/FLAT-20): its app (flat20.c), its MS-20
engine (ms20.h), the panel link's protocol (ms20_link.h), its canvas (gfx.c) and its drawn art and font (gen/)
unchanged, but for one split: app_main's start becomes app_init(), the body of its forever loop app_loop() (a
do { } while (0), so a `continue` is still "the next pass"), its loop-wide variables file-level statics; and one for a
module's RAM: the canvas in .bss (a module's RAM is one region, cleared by the host). Its `#include "platform.c"`
finds flat20/platform.c: fm1-dock's (flat20_platform.inc copied in, see there). Run it again on a new FLAT-20:
    modules/flat20/take.py ~/src/FLAT-20        (FLAT-20 0c96ea5, 0.3.2)"""
import shutil
import sys
from pathlib import Path

src = Path(sys.argv[1]) / "firmware"
out = Path(__file__).resolve().parent / "flat20"
out.mkdir(exist_ok=True)
for f in ["src/ms20.h", "src/ms20_link.h", "src/gfx.c", "gen/felucca_font.h", "gen/panel_art.h"]:
    shutil.copy(src / f, out / Path(f).name)
shutil.copy(src.parent / "LICENSE", out / "LICENSE")

POOL = ' __attribute__((section(".pool")))'
g = (out / "gfx.c").read_text()
old = "static uint16_t cv_px[CV_MAX]" + POOL + ";"
assert g.count(old) == 1
(out / "gfx.c").write_text(g.replace(old, "static uint16_t cv_px[CV_MAX];                 /* fm1-dock: one RAM region (take.py) */"))

s = (src / "src/flat20.c").read_text()


def sub(old, new):
    global s
    assert s.count(old) == 1, (old[:70], s.count(old))
    s = s.replace(old, new)


head = '''static void app_main(void)
{
    uint32_t prev = 0, k, page = 4, sel = 0, redraw = 7, t_change = 0, set_dirty = 0, bank_dirty = 0, t_cpu = 0, pct_drawn = ~0u;
    uint32_t lamps_drawn = ~0u, lamps, pct = 0, t_graph = 0, flash_until = 0;
    const char *flash = 0;                      /* the preset's name over the graph for a moment: why ("PROGRAM", "SAVED") */
    /* the splash stays 1.5 s, or until something is played or turned; only the screen waits: the sound, MIDI and the
     * panel link run from the start */
    uint32_t splash = 1, t_boot, page0, preset0;
    int32_t wheel0;
    uint32_t rec_midi = 0;
    uint8_t key_note[27] = {0};
'''
sub(head, '''/* fm1-dock: app_main's loop-wide variables, for app_init and app_loop (modules/flat20/take.py) */
static uint32_t prev = 0, k, page = 4, sel = 0, redraw = 7, t_change = 0, set_dirty = 0, bank_dirty = 0, t_cpu = 0, pct_drawn = ~0u;
static uint32_t lamps_drawn = ~0u, lamps, pct = 0, t_graph = 0, flash_until = 0;
static const char *flash = 0;
static uint32_t splash = 1, t_boot, page0, preset0;
static int32_t wheel0;
static uint32_t rec_midi = 0;
static uint8_t key_note[27] = {0};
static void app_init(void)                      /* fm1-dock: app_main up to its loop */
{
''')
sub('''    wheel0 = ctl.wheel;

    for (;;) {
''', '''    wheel0 = ctl.wheel;
}

static void app_loop(void)                      /* fm1-dock: a pass of app_main's loop, from the host's draw() */
{
    do {
''')
assert s.rstrip().endswith("    }\n}"), s[-40:]
s = s.rstrip()[:-len("    }\n}")] + "    } while (0);\n}\n"
(out / "flat20.c").write_text("/* fm1-dock: FLAT-20's firmware/src/flat20.c, split by modules/flat20/take.py: do not edit */\n" + s)
shutil.copy(Path(__file__).resolve().parent / "flat20_platform.inc", out / "platform.c")
print("flat20/: taken from", src.parent)
