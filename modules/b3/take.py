#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""modules/b3/b3/ from B3 ORGAN's own tree (github.com/R-Jeb/setBfree-fm1): its organ (organ_*.h: the wheels,
setBfree's tapering, leakage, programs and controllers, the scanner, percussion, overdrive, reverb, Leslie, equalizer and
logo) and its app unchanged, but for one split: dinghy.c's app_main becomes app_init(), the body of its forever loop
app_loop() (a do { } while (0), so a `continue` is still "the next pass"), its loop-wide variables file-level statics
(found by themselves: every declaration before app_main's first statement); and for a module's RAM: the delay lines'
ORGAN_POOL and gfx.c's .pool in .bss (a module's RAM is one region, cleared by the host). The battery in the corner goes:
a module has no battery reading. Its `#include "platform.c"` finds b3/platform.c: fm1-dock's (b3_platform.inc copied in:
the settings and the user's programs in the module's settings record and flash bank, no web editor). Run it again on a
new B3 ORGAN (tools/modules_check.py --try b3 does):
    modules/b3/take.py ~/src/setBfree-fm1        (B3 ORGAN v0.6.1, 7c5c0b8)"""
import re
import shutil
import sys
from pathlib import Path

src = Path(sys.argv[1]) / "firmware"
out = Path(__file__).resolve().parent / "b3"
out.mkdir(exist_ok=True)
for f in sorted((src / "src").glob("organ_*.h")) + [src / "src/gfx.c", src / "gen/felucca_font.h"]:
    shutil.copy(f, out / f.name)
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
# app_main's variables (every declaration before its first statement) become file-level statics; what else comes before
# (a test's #ifdef) stays at the top of app_init
A = "static void app_main(void)\n{\n"
assert s.count(A) == 1
i = s.index(A) + len(A)
body = s[i:]
decl, rest, k = [], [], 0
lines = body.split("\n")
TYPE = re.compile(r"^    (struct\b|u?int(8|16|32|64)_t\b|int\b|unsigned\b|char\b|float\b|const\b)")
while k < len(lines):
    ln = lines[k]
    if not ln.strip():
        k += 1
        if decl and not (k < len(lines) and (TYPE.match(lines[k]) or lines[k].startswith("#"))):
            break
        continue
    if ln.startswith("#"):                       # a preprocessor block: kept for app_init, with what it holds
        j = k
        while not lines[j].startswith("#endif"):
            j += 1
        rest += lines[k:j + 1]
        k = j + 1
        continue
    if TYPE.match(ln):
        j, depth = k, 0                          # (a struct over several lines: up to its closing brace's ;)
        while True:
            code = lines[j].split("/*")[0]
            depth += code.count("{") - code.count("}")
            if depth == 0 and code.rstrip().endswith(";"):
                break
            j += 1
        decl.append("\n".join(lines[k:j + 1]))
        k = j + 1
        continue
    break
assert decl, "no declarations at app_main's top"
statics = "\n".join("static " + d.lstrip() if not d.lstrip().startswith("static") else d.lstrip() for d in decl)
s = (s[:s.index(A)] + "/* fm1-dock: app_main's loop-wide variables, for app_init and app_loop (modules/b3/take.py) */\n" + statics +
     "\nstatic void app_init(void)                      /* fm1-dock: app_main up to its loop */\n{\n" +
     ("\n".join(rest) + "\n" if rest else "") + "\n".join(lines[k:]))
L = "\n    for (;;) {\n"
assert s.count(L) >= 1
j = s.index(L, s.index("static void app_init(void)"))
s = s[:j] + "\n}\n\nstatic void app_loop(void)                      /* fm1-dock: a pass of app_main's loop, from the host's draw() */\n{\n    do {\n" + s[j + len(L):]
# the battery: none in a module
m = re.search(r"uint32_t bs = battery_state\(\), ", s)
if m:
    s = s[:m.start()] + "uint32_t " + s[m.end():]
    s = re.sub(r"\n\s*if \(bs != drawn_batt[^\n]*\n", "\n            (void)draw_battery; (void)drawn_batt;   /* fm1-dock: no battery reading in a module */\n", s, count=1)
else:
    sub('''        {
            uint32_t bs = battery_state();
            if (bs != drawn_batt) { drawn_batt = bs; draw_battery(bs); }
        }
''', '''        (void)draw_battery;                     /* fm1-dock: no battery reading in a module */
''')
# the logo's drawbars where the organ's are (gen_logo_bars.py makes logo_bars.h from organ_logo.h)
OLD_LOGO_HEAD = "static void draw_logo(void)                     /* in strips as high as the canvas holds (CV_MAX: 64 rows on the FM-1, more on the PC) */\n{"
NEW_LOGO_HEAD = '''#include "../logo_bars.h"                       /* fm1-dock: the drawbars where the organ's are (modules/b3/gen_logo_bars.py) */
static const uint8_t *logo_db(void) { return P.keys % 3u == 1u ? (const uint8_t *)P.dbl : (const uint8_t *)P.db; }
static uint32_t logo_key(void)                  /* the drawbars the logo shows, never 0 (logo_drawn's "draw it") */
{
    uint32_t i, k = 0;
    const uint8_t *v = logo_db();
    for (i = 0; i < 9u; i++) k = k * 9u + (v[i] > 8u ? 8u : v[i]);
    return k + 1u;
}
static void logo_spr(uint32_t s, int32_t px, int32_t py, uint32_t r0)   /* a bar's piece into the strip from row r0 */
{
    const lb_spr_t *q = &LB_SPR[s];
    const uint8_t *a = LB_PX + q->at;
    int32_t sx, sy;
    for (sy = 0; sy < (int32_t)q->h; sy++) {
        int32_t y = py + sy;
        if (y < (int32_t)r0 || y >= (int32_t)(r0 + cv_h)) continue;
        for (sx = 0; sx < (int32_t)q->w; sx++) {
            uint32_t c = a[sy * (int32_t)q->w + sx];
            int32_t x = px + sx;
            if (c == LB_T || x < 0 || x >= 240 || 100 * x + 188 * y < (int32_t)LB_K) continue;   /* (behind the band) */
            cv_px[(uint32_t)(y - (int32_t)r0) * 240u + (uint32_t)x] = swap16(LOGO_PAL[c]);
        }
    }
}
''' + OLD_LOGO_HEAD
OLD_DEC = '''                if (i >= sizeof LOGO_RLE) break;
                c = LOGO_RLE[i] >> 4; left = (LOGO_RLE[i] & 15u) + 1u; i++;'''
NEW_DEC = '''                if (i >= sizeof LB_BG) break;      /* fm1-dock: the logo without its bars */
                c = LB_BG[i] >> 4; left = (LB_BG[i] & 15u) + 1u; i++;'''
OLD_VER = '''        if (r0 == 0) cv_text(6, 4, &FONT_S, "v" APP_VER, RGB(150, 128, 104));'''
NEW_VER = '''        {                                       /* fm1-dock: the bars where the organ's drawbars are */
            const uint8_t *v = logo_db();
            uint32_t b;
            (void)LOGO_RLE;
            for (b = 0; b < 9u; b++) {
                uint32_t n = v[b] > 8u ? 8u : v[b];
                logo_spr(0, LB_OFF[b][n][0], LB_OFF[b][n][1], r0);
                logo_spr(1u + b, LB_OFF[b][n][2], LB_OFF[b][n][3], r0);
            }
        }
''' + OLD_VER
OLD_DRAW = "            if (!logo_drawn) { logo_drawn = 1; draw_logo(); }"
NEW_DRAW = "            if (logo_drawn != logo_key()) { logo_drawn = logo_key(); draw_logo(); }   /* (fm1-dock: again when a drawbar moves) */"
LOGO_SUBS = [(OLD_LOGO_HEAD, NEW_LOGO_HEAD), (OLD_DEC, NEW_DEC), (OLD_VER, NEW_VER), (OLD_DRAW, NEW_DRAW)]
for _o, _n in LOGO_SUBS:
    sub(_o, _n)
# a press while the logo shows wakes it and does its job (one click, not two)
sub('            if (any) { logo_on = 0; logo_drawn = 0; screen = 0; drawn_screen = 99; t_input = fm1_ms; b = 0; }',
    '            if (any) { logo_on = 0; logo_drawn = 0; screen = 0; drawn_screen = 99; t_input = fm1_ms; }   /* (fm1-dock: the press that wakes it does its job too) */')
assert s.rstrip().endswith("    }\n}"), s[-40:]
s = s.rstrip()[:-len("    }\n}")] + "    } while (0);\n}\n"
(out / "b3.c").write_text("/* fm1-dock: B3 ORGAN's firmware/src/dinghy.c, split by modules/b3/take.py: do not edit */\n" + s)
shutil.copy(Path(__file__).resolve().parent / "b3_platform.inc", out / "platform.c")
print("b3/: taken from", src.parent)
