#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""modules/artihive/artihive/ from ARTIHIVE's own tree (KHAV's, on Dinghy): its app and DSP unchanged, but
for one split: app_main's start becomes app_init(), the body of its forever loop app_loop() (a do { } while (0),
so a `continue` is still "the next pass"), its loop-wide variables file-level statics, and its boot screen's
3.6 s hold and 0.35 s wait gone (a module's init must return); and for a module's
272 KiB of RAM: the .pool sections in .bss (a module's RAM is one region, cleared by the host), the delay lines'
length DFRAMES and the TIME law's top DELAY_MAX_FRAMES settable (module.cfg: ~1 s instead of 2.04 s), and the
six textured themes' patina maps (gen/*_texture*.h, 86 KB of flash) left out: those themes are their plain
colour field. Its `#include "platform.c"` finds artihive/platform.c: fm1-dock's (artihive_platform.inc copied
in, see there). Run it again on a new ARTIHIVE:
    modules/artihive/take.py ~/src/artihive        (ARTIHIVE 2.0.3, ed53f4b)"""
import shutil
import sys
from pathlib import Path

src = Path(sys.argv[1]) / "firmware"
out = Path(__file__).resolve().parent / "artihive"
out.mkdir(exist_ok=True)
for f in ["gen/artihive_tables.h", "gen/felucca_font.h"]:
    shutil.copy(src / f, out / Path(f).name)
for f in ["LICENSE", "LICENSING.md"]:
    shutil.copy(src.parent / f, out / f)


def sub(t, old, new, n=1):
    assert t.count(old) == n, (old[:70], t.count(old))
    return t.replace(old, new)


# the delay's geometry, settable
t = (src / "src/artihive_dsp.h").read_text()
for name in ("DFRAMES", "DELAY_MAX_FRAMES"):
    line = next(x for x in t.splitlines() if x.startswith(f"#define {name} "))
    t = sub(t, line + "\n", f"#ifndef {name}                                  /* fm1-dock: settable (modules/artihive/take.py) */\n"
            + line + "\n#endif\n")
(out / "artihive_dsp.h").write_text(t)

# the canvas: no patina maps (the themes' plain fields), its RAM in .bss
g = (src / "src/gfx.c").read_text()
for h in ("oxide_texture.h", "blush_texture.h", "pastel_textures.h"):
    g = sub(g, f'#include "{h}"\n', "")
g = sub(g, ' __attribute__((section(".pool")))', "")
a = g.index("static void cv_oxide(uint32_t ox, uint32_t oy)\n{")
b = g.index("\n}\n", a) + 3
g = g[:a] + ("static void cv_oxide(uint32_t ox, uint32_t oy)   /* fm1-dock: the patina maps left out (take.py) */\n"
             "{\n    (void)ox;\n    (void)oy;\n}\n") + g[b:]
(out / "gfx.c").write_text(g)

s = (src / "src/artihive.c").read_text()
s = sub(s, '__attribute__((section(".pool"), aligned(4)))', "__attribute__((aligned(4)))")
s = sub(s, '''static void app_main(void)
{
    uint32_t prev_keys = 0, t_draw = 0, save_hold = 0, mutate_hold = 0, theme_hold = 0;
    uint8_t save_suppress = 0;
    uint32_t k;
''', '''/* fm1-dock: app_main's loop-wide variables, for app_init and app_loop (modules/artihive/take.py) */
static uint32_t prev_keys = 0, t_draw = 0, save_hold = 0, mutate_hold = 0, theme_hold = 0;
static uint8_t save_suppress = 0;
static uint32_t k;
static void app_init(void)                      /* fm1-dock: app_main up to its loop */
{
''')
s = sub(s, "        { uint32_t ready_ms = fm1_ms; while (fm1_ms - ready_ms < 350u) platform_poll(); }\n", "")
s = sub(s, "        while (fm1_ms - boot_ms < BOOT_HOLD_MS) {\n",
        "        while (0) {                             /* fm1-dock: no boot hold (a module's init returns) */\n")
s = sub(s, '''    draw_status();

    for (;;) {
''', '''    draw_status();
}

static void app_loop(void)                      /* fm1-dock: a pass of app_main's loop, from the host's draw() */
{
    do {
''')
assert s.rstrip().endswith("    }\n}"), s[-40:]
s = s.rstrip()[:-len("    }\n}")] + "    } while (0);\n}\n"
(out / "artihive.c").write_text("/* fm1-dock: ARTIHIVE's firmware/src/artihive.c, split by modules/artihive/take.py: do not edit */\n" + s)
shutil.copy(Path(__file__).resolve().parent / "artihive_platform.inc", out / "platform.c")
print("artihive/: taken from", src.parent)
