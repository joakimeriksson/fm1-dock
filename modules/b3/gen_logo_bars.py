#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""B3's logo with its nine drawbars where the organ's are (logo_bars.h, from b3/organ_logo.h: R-Jeb's art, unchanged).

The logo's bars are one drawbar moved along two straight lines: the band (23 px from bar to bar) and the bar itself
(8.03 px a step: a cap's top is 86 + 8.03 n from the band at setting n; the art's bars are at 8 8 6 8 3 0 0 2 4). So:
the logo without its bars (below the band, back to the brown), the longest bar's shaft (bar 1, at 8), each bar's own cap,
and where each goes at each setting, whole pixels: the C draws them over the background, hidden behind the band.

  gen_logo_bars.py                  writes logo_bars.h
  gen_logo_bars.py --preview a.png 888000000 ...   also draws those settings (one image each, side by side)
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "b3" / "organ_logo.h"
OUT = HERE / "logo_bars.h"

W = 240
SLOPE = 1.88                     # the band's lower edge: x = 169.75 - 1.88 y
NORM = (1 + SLOPE * SLOPE) ** 0.5
D = (1 / NORM, SLOPE / NORM)     # along a bar, out of the band
E = (SLOPE / NORM, -1 / NORM)    # along the band, bar to bar
C0, PITCH = -13.5, 23.0          # bar i's middle across the band: C0 + PITCH i
U0, STEP = 86.0, 8.03            # a cap's top at setting n: U0 + STEP n
CLIP = 84.0                      # a bar shows from here out (the band's edge, and its lip, cover the rest)
ART = [8, 8, 6, 8, 3, 0, 0, 2, 4]
BOTTOM = 176                     # the bars never reach the title
T = 16                           # a sprite's transparent pixel


def load():
    s = SRC.read_text()
    pal = [int(x, 16) for x in re.search(r"LOGO_PAL\[16\] = \{([^}]*)\}", s).group(1).split(",")]
    body = s[s.index("LOGO_RLE"):].split("{", 1)[1].split("}")[0]
    px = []
    for b in (int(x) for x in re.findall(r"\d+", body)):
        px += [b >> 4] * ((b & 15) + 1)
    return pal, [px[y * W:(y + 1) * W] for y in range(W)]


def u_of(x, y):
    return x * D[0] + y * D[1]


def v_of(x, y):
    return x * E[0] + y * E[1]


def cap_top(n):
    return U0 + STEP * n


def build():
    pal, g = load()
    bgc = 13                                      # the brown
    assert all(g[170][x] == bgc for x in range(5)), "the logo's brown is not index 13"
    clip = lambda x, y: u_of(x, y) >= CLIP
    bg = [row[:] for row in g]
    for y in range(BOTTOM):
        for x in range(W):
            if clip(x, y):
                bg[y][x] = bgc

    def sprite(pred):
        pts = [(x, y, g[y][x]) for y in range(BOTTOM) for x in range(W) if clip(x, y) and g[y][x] != bgc and pred(x, y)]
        x0, y0 = min(p[0] for p in pts), min(p[1] for p in pts)
        w, h = max(p[0] for p in pts) - x0 + 1, max(p[1] for p in pts) - y0 + 1
        a = [T] * (w * h)
        for x, y, c in pts:
            a[(y - y0) * w + x - x0] = c
        return x0, y0, w, h, a

    slot = lambda i, half: lambda x, y: abs(v_of(x, y) - (C0 + PITCH * i)) < half
    t0 = cap_top(ART[0])
    shaft = sprite(lambda x, y: slot(0, 7.5)(x, y) and u_of(x, y) < t0 + 3)
    caps = [sprite(lambda x, y, i=i: slot(i, 12.5)(x, y) and cap_top(ART[i]) - 2.5 <= u_of(x, y) <= cap_top(ART[i]) + 32)
            for i in range(9)]
    off = []
    for i in range(9):
        row = []
        for n in range(9):
            du = cap_top(n) - t0
            dv = PITCH * i
            sx = round(du * D[0] + dv * E[0]) + shaft[0]
            sy = round(du * D[1] + dv * E[1]) + shaft[1]
            du = cap_top(n) - cap_top(ART[i])
            cx = round(du * D[0]) + caps[i][0]
            cy = round(du * D[1]) + caps[i][1]
            row.append((sx, sy, cx, cy))
        off.append(row)
    return pal, g, bg, shaft, caps, off


def render(bg, shaft, caps, off, db):
    im = [row[:] for row in bg]

    def put(spr, px, py):
        _, _, w, h, a = spr
        for sy in range(h):
            for sx in range(w):
                c = a[sy * w + sx]
                x, y = px + sx, py + sy
                if c != T and 0 <= x < W and 0 <= y < W and 100 * x + 188 * y >= K:
                    im[y][x] = c
    for i in range(9):
        o = off[i][db[i]]
        put(shaft, o[0], o[1])
        put(caps[i], o[2], o[3])
    return im


K = round(CLIP * NORM * 100)                     # 100 x + 188 y >= K: out of the band (the C's test)


def rle(rows):
    flat = [c for r in rows for c in r]
    out, i = [], 0
    while i < len(flat):
        n = 1
        while i + n < len(flat) and flat[i + n] == flat[i] and n < 16:
            n += 1
        out.append(flat[i] << 4 | (n - 1))
        i += n
    return out


def main():
    pal, g, bg, shaft, caps, off = build()
    assert render(bg, shaft, caps, off, ART) is not None
    sprites = [shaft] + caps
    px, starts = [], []
    for s in sprites:
        starts.append(len(px))
        px += s[4]
    b = rle(bg)
    lines = ["/* SPDX-License-Identifier: GPL-3.0-only */",
             "/* B3's logo with its drawbars where the organ's are (gen_logo_bars.py, from organ_logo.h: do not edit) */",
             f"#define LB_K {K}u                        /* 100 x + 188 y >= LB_K: below the band, where a bar shows */",
             "#define LB_T 16u                          /* a sprite's see-through pixel */",
             f"static const uint8_t LB_BG[{len(b)}] = {{"]
    for i in range(0, len(b), 24):
        lines.append("    " + ", ".join(map(str, b[i:i + 24])) + ",")
    lines.append("};")
    lines.append("typedef struct { uint8_t w, h; uint16_t at; } lb_spr_t;")
    lines.append("static const lb_spr_t LB_SPR[10] = {   /* 0 the shaft, 1..9 bar i's cap */")
    lines.append("    " + ", ".join(f"{{{s[2]}, {s[3]}, {a}}}" for s, a in zip(sprites, starts)) + ",")
    lines.append("};")
    lines.append(f"static const uint8_t LB_PX[{len(px)}] = {{")
    for i in range(0, len(px), 32):
        lines.append("    " + ", ".join(map(str, px[i:i + 32])) + ",")
    lines.append("};")
    lines.append("static const int16_t LB_OFF[9][9][4] = {   /* [bar][setting]: the shaft's top left, the cap's */")
    for row in off:
        lines.append("    {" + ", ".join("{%d, %d, %d, %d}" % o for o in row) + "},")
    lines.append("};")
    OUT.write_text("\n".join(lines) + "\n")
    print(f"{OUT.name}: background {len(b)} B, sprites {len(px)} B")
    if "--preview" in sys.argv:
        from PIL import Image
        a = sys.argv[sys.argv.index("--preview") + 1:]
        path, sets = a[0], a[1:] or ["".join(map(str, ART))]
        rgb = lambda c: (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)
        out = Image.new("RGB", (W * len(sets) + 10 * (len(sets) - 1), W), (0, 0, 0))
        for k, s in enumerate(sets):
            im = render(bg, shaft, caps, off, [int(ch) for ch in s])
            one = Image.new("RGB", (W, W))
            one.putdata([rgb(pal[c]) for r in im for c in r])
            out.paste(one, (k * (W + 10), 0))
        out = out.resize((out.width * 2, out.height * 2), Image.NEAREST)
        out.save(path)
        same = render(bg, shaft, caps, off, ART) == g
        print("preview:", path, "| the art's own settings redraw the art exactly" if same else "| the art's settings differ from the art")


if __name__ == "__main__":
    main()
