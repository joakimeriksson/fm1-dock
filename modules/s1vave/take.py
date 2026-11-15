#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""modules/s1vave/s1vave/ from S1Vave's own tree (github.com/AndrewGG88/S1Vave): its app and DSP unchanged but
for what a module needs, as OberVave's (modules/obervave/take.py; S1Vave grew from OberVave):
  - app_main split: its start app_init(), the body of its forever loop app_loop() (a do { } while (0), so a
    `continue` is still "the next pass"), its loop-wide variables file-level statics;
  - its .pool arrays in .bss (a module's RAM is one region, cleared by the host), the delay's DLY_MAX settable;
  - not taken: Roland's S-1 factory presets (obv_patches.h, "Authentic Roland AIRA Compact S-1 Factory
    Presets", from Roland's .PRM backups): fm1-dock ships no maker's bank. s1v_patches.h is fm1-dock's own
    bank instead (NFACT 16), with the same programs' room (99) for the user's;
  - not taken: Andrew GG's logo and splash (logo_s1vave.h, splash_s1vave.h: TRADEMARK.md, a repackaged firmware
    leaves them out): s1v_brand.inc draws the name in text, and the splash is a short text one that names the
    author and says this is fm1-dock's port;
  - no battery icon (the host has no battery reading for a module).
Its `#include "platform.c"` finds s1vave/platform.c: fm1-dock's (s1v_platform.inc copied in). Again on a new S1Vave:
    modules/s1vave/take.py ~/src/S1Vave        (S1Vave e837018, v2.0.1)"""
import shutil
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
src = Path(sys.argv[1]) / "firmware"
out = here / "s1vave"
out.mkdir(exist_ok=True)
for f in ["src/obv_dsp.h", "src/obv_params.h", "src/obv_fx.h", "src/obv_engine.h", "src/obv_seq.h", "src/obv_ui.h",
          "src/gfx.c", "gen/felucca_font.h", "gen/obv_tables.h"]:
    shutil.copy(src / f, out / Path(f).name)
for f in ["LICENSE", "LICENSING.md", "TRADEMARK.md"]:
    shutil.copy(src.parent / f, out / f)


def edit(name, pairs):
    p = out / name
    t = p.read_text()
    for old, new in pairs:
        assert t.count(old) == 1, (name, old[:70], t.count(old))
        t = t.replace(old, new)
    p.write_text(t)


POOL = ' __attribute__((section(".pool")))'
edit("gfx.c", [(POOL, "")])
edit("obv_fx.h", [(POOL, ""), ('#define DLY_MAX 32768u                          /* a side of the delay: 743 ms */',
                               '#ifndef DLY_MAX                                  /* fm1-dock: settable (modules/s1vave/take.py) */\n'
                               '#define DLY_MAX 32768u                          /* a side of the delay: 743 ms */\n#endif')])
edit("obv_params.h", [("#define NFACT 64u", "#define NFACT 16u                               /* fm1-dock: its own bank (s1v_patches.h) */")])
ui = (out / "obv_ui.h").read_text()
a, b = ui.index("/* S1VAVE Boot Splash:"), ui.index("static void ui_splash(void)")
end = ui.index("\n}\n", b) + 3
ui = ui[:a] + ui[end:]                          # (the splash: s1v_brand.inc's)
(out / "obv_ui.h").write_text(ui)
edit("obv_ui.h", [('#include "logo_s1vave.h"', '#include "s1v_brand.inc"                      /* fm1-dock: the name in text (take.py) */'),
                  ("    draw_battery_icon(218, 26, platform_battery_bars());\n", "")])

s = (src / "src/s1vave.c").read_text()


def sub(old, new):
    global s
    assert s.count(old) == 1, (old[:70], s.count(old))
    s = s.replace(old, new)


sub('''static void app_main(void)
{
    uint32_t k, i, t_led = 0, t_scope = 0, t_page = 0, t_hdr = 0, leds_q = 1;
    uint32_t swap_seen = 0, leds_prev = 0xFFFFFFFFu, glow = 0, glow_prev = 0xFFFFFFFFu, t_steps = 0, ticks_seen = 0;
''', '''/* fm1-dock: app_main's loop-wide variables, for app_init and app_loop (modules/s1vave/take.py) */
static uint32_t k, i, t_led = 0, t_scope = 0, t_page = 0, t_hdr = 0, leds_q = 1;
static uint32_t swap_seen = 0, leds_prev = 0xFFFFFFFFu, glow = 0, glow_prev = 0xFFFFFFFFu, t_steps = 0, ticks_seen = 0;
static void app_init(void)                      /* fm1-dock: app_main up to its loop */
{
''')
sub('''    ui_splash();
    scope_state = 1;

    for (;;) {
''', '''    ui_splash();
    scope_state = 1;
}

static void app_loop(void)                      /* fm1-dock: a pass of app_main's loop, from the host's draw() */
{
    do {
''')
sub('#include "obv_patches.h"', '#include "s1v_patches.h"                       /* fm1-dock: its own bank, not Roland\'s (take.py) */')
assert s.rstrip().endswith("    }\n}"), s[-40:]
s = s.rstrip()[:-len("    }\n}")] + "    } while (0);\n}\n"
s = s.replace(POOL, "")
(out / "s1vave.c").write_text("/* fm1-dock: S1Vave's firmware/src/s1vave.c, split by modules/s1vave/take.py: do not edit */\n" + s)
shutil.copy(here / "s1v_platform.inc", out / "platform.c")
shutil.copy(here / "s1v_patches.h", out / "s1v_patches.h")
shutil.copy(here / "s1v_brand.inc", out / "s1v_brand.inc")
print("s1vave/: taken from", src.parent)
