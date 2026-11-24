#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""modules/vector37/vector37/ from Vector37's own tree (github.com/genkigenki/Vector37, M. Geisler): its engine, orbit,
effects, sounds, UI and screen, with its FM-1 panel (panel_fm1.c), unchanged but for the few edits below; its tables
and font made by its own generators (tools/gen_tables.py, tools/gen_font.py: Pillow). The platform under it is
fm1-dock's (v37_platform.inc). Its wavetables (8 banks x 32 frames, 200 KB at 16 bits) do not go in the module: they
become the data pack V37-WT (hb_module.h "Data packs"), modules/vector37/v37wt.bin, 8 bits with a scale per frame and
level (or 16, --bits 16), which the module finds by name. Run it again on a new Vector37:
    uv run --with Pillow modules/vector37/take.py ~/work/vector37 [--bits 8|16]      (Vector37 08742a4)
then tools/hbmod.py modules/vector37 and tools/hbmod.py --pack V37-WT modules/vector37/v37wt.bin.

The edits, each asserted (the take fails if Vector37 changed there):
  - libc.c: its memset / memcpy / memcmp go (the module has modules/lib/hb_lib.c's);
  - the .pool section (Felucca's second RAM region) is gone: a module's RAM is one region the host clears;
  - fx.c: the delay and the reverb go (their 195 KB of RAM; the dock's own effects play under it: OPTIONS, EFFECT);
    drive and chorus stay;
  - engine.c: wt_load reads a frame from the pack (v37_wt_*, the platform's) instead of WT_DATA; no pack: silence;
  - scenes.c: the FM-1 build kept no store; here the store is the host's (settings and the USER bank: the shim);
  - panel_fm1.c: no settling wait on a key scan (the host's keys come settled)."""
import argparse
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("src", type=Path)
ap.add_argument("--bits", type=int, choices=(8, 16), default=8)
a = ap.parse_args()
src = a.src.resolve()
here = Path(__file__).resolve().parent
out = here / "vector37"
out.mkdir(exist_ok=True)
FILES = ["synth.h", "libc.c", "gfx.c", "orbit.c", "engine.c", "analog.c", "fx.c", "mix.c", "params.c", "presets.c",
         "scenes.c", "ui.c", "panel_fm1.c", "draw.c"]
for f in FILES:
    shutil.copy(src / "firmware" / "src" / f, out / f)
for f, to in [("LICENSE", "LICENSE"), ("README.md", "README.md"), ("assets/fonts/Terminus-LICENSE.txt", "Terminus-LICENSE.txt")]:
    shutil.copy(src / f, out / to)
def revision(d):
    """the checkout's commit, short: by git, else from its files (a Python that cannot run git: uv's under Rosetta)"""
    r = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True, cwd=d).stdout.strip()
    if r:
        return r
    try:
        head = (d / ".git" / "HEAD").read_text().strip()
        if not head.startswith("ref: "):
            return head[:7]
        ref = head[5:]
        if (d / ".git" / ref).exists():
            return (d / ".git" / ref).read_text().strip()[:7]
        for ln in (d / ".git" / "packed-refs").read_text().splitlines():
            if ln.endswith(" " + ref):
                return ln[:7]
    except OSError:
        pass
    return "?"


rev = revision(src)

# the generated headers, by Vector37's own generators (into a scratch place, then taken)
gen = here / "_gen"
gen.mkdir(exist_ok=True)
subprocess.run([sys.executable, str(src / "tools" / "gen_font.py"), str(gen / "felucca_font.h")], check=True, cwd=src)
subprocess.run([sys.executable, str(src / "tools" / "gen_tables.py"), str(gen / "mk_tables.h")], check=True, cwd=src)
shutil.copy(gen / "felucca_font.h", out / "felucca_font.h")
tables = (gen / "mk_tables.h").read_text()
shutil.rmtree(gen)


def sub(name, old, new, count=1):
    t = (out / name).read_text()
    assert t.count(old) == count, (name, old[:80], t.count(old))
    (out / name).write_text(t.replace(old, new))


# ---- the wavetables: out of the module, into the pack
m = re.search(r"static const int16_t WT_DATA\[(\d+)\] = \{(.*?)\};\n", tables, re.S)
assert m, "mk_tables.h: no WT_DATA"
wt = [int(x) for x in re.findall(r"-?\d+", m.group(2))]
assert len(wt) == int(m.group(1))
tables = tables[:m.start()] + "/* WT_DATA: in the data pack V37-WT (modules/vector37/take.py) */\n" + tables[m.end():]


def const(name):
    return int(re.search(rf"#define {name} (\d+)", tables).group(1))


def arr(name):
    return [int(x) for x in re.findall(r"\d+", re.search(rf"{name}\[\d+\] = \{{(.*?)\}};", tables, re.S).group(1))]


NB, NF, FR, NL = const("WT_NB"), const("WT_NF"), const("WT_FR"), const("WT_NLVL")
OFF, N = arr("WT_LVL_OFF"), arr("WT_LVL_N")
assert len(wt) == NB * NF * FR
# the pack: "V3WT", bits, NB, NF, NL, FR, then (8 bits) a scale per frame and level (u16: x = q * mul >> 7), the samples
head = struct.pack("<4sBBBBH", b"V3WT", a.bits, NB, NF, NL, FR) + struct.pack(f"<{NL}H", *OFF) + struct.pack(f"<{NL}H", *N)
if a.bits == 16:
    body = struct.pack(f"<{len(wt)}h", *wt)
else:
    muls, q = [], bytearray(len(wt))
    for f in range(NB * NF):
        for lv in range(NL):
            at = f * FR + OFF[lv]
            seg = wt[at:at + N[lv]]
            peak = max(abs(x) for x in seg)
            mul = round(peak * 128 / 127)
            muls.append(mul)
            for i, x in enumerate(seg):
                q[at + i] = (round(x * 127 / peak) if peak else 0) & 0xFF
    body = struct.pack(f"<{len(muls)}H", *muls) + bytes(q)
pack = head + body
(here / "v37wt.bin").write_bytes(pack)
(out / "mk_tables.h").write_text(tables)

# ---- the edits
sub("libc.c", "void *memset(void *d, int c, unsigned n)\n{", "#if 0   /* fm1-dock: memset, memcpy, memcmp are modules/lib/hb_lib.c's (take.py) */\nvoid *memset(void *d, int c, unsigned n)\n{")
sub("libc.c", "            return *p - *q;\n    return 0;\n}\n", "            return *p - *q;\n    return 0;\n}\n#endif\n")
for f in ["engine.c", "fx.c", "draw.c"]:
    t = (out / f).read_text()
    assert ' __attribute__((section(".pool")))' in t, f
    (out / f).write_text(t.replace(' __attribute__((section(".pool")))', ""))
sub("gfx.c", '__attribute__((section(".pool"), aligned(4)))', "__attribute__((aligned(4)))")
sub("fx.c", "/* ------------------------------------------------------------- delay --- */",
    "#if !V37_NO_DELAY_REVERB   /* fm1-dock: the delay and the reverb out (take.py): the dock's effects instead */\n"
    "/* ------------------------------------------------------------- delay --- */")
sub("fx.c", "static void fx_run(int32_t *l, int32_t *r)\n{", "#endif\nstatic void fx_run(int32_t *l, int32_t *r)\n{")
sub("fx.c", "    delay_run(l, r);\n    reverb_run(l, r);\n",
    "#if !V37_NO_DELAY_REVERB\n    delay_run(l, r);\n    reverb_run(l, r);\n#endif\n")
sub("engine.c", """    const int16_t *s = WT_DATA + (bank * WT_NF + k) * WT_FR + WT_LVL_OFF[lvl];
    uint32_t i, n = WT_LVL_N[lvl];
    for (i = 0; i < n; i++)
        d[i] = s[i];
    d[n] = s[0];                           /* (the guard: tlu reads one past) */""",
    """    uint32_t i, n = WT_LVL_N[lvl], at = (bank * WT_NF + k) * WT_FR + WT_LVL_OFF[lvl];   /* fm1-dock: from the pack */
    if (!v37_wt_s8 && !v37_wt_s16)                          /* (no pack: a silent frame; take.py) */
        for (i = 0; i < n; i++)
            d[i] = 0;
    else if (v37_wt_s16)
        for (i = 0; i < n; i++)
            d[i] = v37_wt_s16[at + i];
    else {
        int32_t m = v37_wt_mul[(bank * WT_NF + k) * WT_NLVL + lvl];
        for (i = 0; i < n; i++)
            d[i] = (int16_t)((v37_wt_s8[at + i] * m) >> 7);
    }
    d[n] = d[0];                           /* (the guard: tlu reads one past) */""")
sub("draw.c", "    const int16_t *a = WT_DATA + (bank % WT_NB * WT_NF + (uint32_t)(pos >> 16)) * WT_FR, *b = (pos >> 16) + 1 < WT_NF ? a + WT_FR : a;",
    "    uint32_t ka = bank % WT_NB * WT_NF + (uint32_t)(pos >> 16), kb = (pos >> 16) + 1 < WT_NF ? ka + 1u : ka;   /* fm1-dock: the pack's (take.py) */")
sub("draw.c", "        int32_t j = i * 128 / w, v = a[j] + (((b[j] - a[j]) * f) >> 15), ny = y + h / 2 - v * (h / 2 - 1) / 28000;",
    "        int32_t j = i * 128 / w, va = v37_wt_at(ka, (uint32_t)j), v = va + (((v37_wt_at(kb, (uint32_t)j) - va) * f) >> 15),\n"
    "                ny = y + h / 2 - v * (h / 2 - 1) / 28000;")
sub("scenes.c", """#if !BOARD_SMK37
    return;                                 /* (the FM-1: no store of ours; Felucca's data there stays untouched) */
#endif
""", "")
sub("panel_fm1.c", "    if (fm1_in.frames < 100u)                      /* (the first readings settle) */",
    "    if (0)                                         /* fm1-dock: the host's keys come settled (take.py) */")
for f in FILES:
    t = (out / f).read_text()
    (out / f).write_text(f"/* fm1-dock: Vector37 {rev}'s firmware/src/{f}, taken by modules/vector37/take.py: do not edit */\n" + t)
print(f"vector37/: taken from {src} ({rev}); the pack v37wt.bin: {len(pack)} B, {a.bits} bits")
