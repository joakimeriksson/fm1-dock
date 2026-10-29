#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Joakim Eriksson
"""The master EQ's coefficients (firmware/src/eq_table.h): its three bands at -12..+12 dB in 1 dB steps, as
the AC79's hardware EQ takes a section, [g, c1, c2, c3, c4] (f32 bits):

  H(z) = g (1 + c4 z^-1 + c2 z^-2) / (1 - c3 z^-1 - c1 z^-2)

(fm1-emu's crates/fm1-soc/src/hweq.rs, measured on an FM-1: the SDK's [b0/a0, -a2/a0, b2/b0, -a1/a0, b1/b0]).
The bands are the Audio EQ Cookbook's (R. Bristow-Johnson): a low shelf, a peak and a high shelf. 0 dB is
[1, 0, 0, 0, 0], the section passing its input through as it is.

  tools/eq_table.py            writes firmware/src/eq_table.h
  tools/eq_table.py --check    fails if the file is not what this writes
  (as a module: response(band, db, f) the band's gain at f Hz, for tests/emu_test.py)
"""
import math
import struct
import sys
from pathlib import Path

FS = 44100.0
DB_MAX = 12
BANDS = [("LOW", "low shelf", 150.0, 0.0), ("MID", "peak", 1000.0, 0.9), ("HIGH", "high shelf", 6000.0, 0.0)]
OUT = Path(__file__).resolve().parents[1] / "firmware" / "src" / "eq_table.h"


def biquad(band, db):
    """(b0, b1, b2, a1, a2), a0 = 1"""
    _, kind, f0, q = BANDS[band]
    a = 10 ** (db / 40)
    w = 2 * math.pi * f0 / FS
    cw, sw = math.cos(w), math.sin(w)
    if kind == "peak":
        al = sw / (2 * q)
        b, aa = (1 + al * a, -2 * cw, 1 - al * a), (1 + al / a, -2 * cw, 1 - al / a)
    else:
        al = sw / 2 * math.sqrt(2)               # (shelf slope S = 1)
        r = 2 * math.sqrt(a) * al
        if kind == "low shelf":
            b = (a * ((a + 1) - (a - 1) * cw + r), 2 * a * ((a - 1) - (a + 1) * cw), a * ((a + 1) - (a - 1) * cw - r))
            aa = ((a + 1) + (a - 1) * cw + r, -2 * ((a - 1) + (a + 1) * cw), (a + 1) + (a - 1) * cw - r)
        else:
            b = (a * ((a + 1) + (a - 1) * cw + r), -2 * a * ((a - 1) + (a + 1) * cw), a * ((a + 1) + (a - 1) * cw - r))
            aa = ((a + 1) - (a - 1) * cw + r, 2 * ((a - 1) - (a + 1) * cw), (a + 1) - (a - 1) * cw - r)
    return b[0] / aa[0], b[1] / aa[0], b[2] / aa[0], aa[1] / aa[0], aa[2] / aa[0]


def section(band, db):
    """the hardware EQ's five coefficients"""
    if db == 0:
        return (1.0, 0.0, 0.0, 0.0, 0.0)
    b0, b1, b2, a1, a2 = biquad(band, db)
    return (b0, -a2, b2 / b0, -a1, b1 / b0)


def response(band, db, f):
    """the band's gain (as a factor) at f Hz"""
    if db == 0:
        return 1.0
    b0, b1, b2, a1, a2 = biquad(band, db)
    z = complex(math.cos(2 * math.pi * f / FS), -math.sin(2 * math.pi * f / FS))
    return abs((b0 + b1 * z + b2 * z * z) / (1 + a1 * z + a2 * z * z))


def f32(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def text():
    out = ["/* SPDX-License-Identifier: GPL-3.0-only */",
           "/* the master EQ's sections (tools/eq_table.py writes this; do not edit): EQ_TABLE[band][dB + 12], the",
           " * hardware EQ's [g, c1, c2, c3, c4] as f32 bits. " + ", ".join(
               f"{n}: {k} {f:g} Hz" + (f" Q {q:g}" if q else "") for n, k, f, q in BANDS) + " */",
           f"#define EQ_BANDS {len(BANDS)}u", f"#define EQ_DB_MAX {DB_MAX}",
           f"static const uint32_t EQ_TABLE[EQ_BANDS][{2 * DB_MAX + 1}][5] = {{"]
    for band, (name, _, _, _) in enumerate(BANDS):
        out.append(f"    {{   /* {name} */")
        for db in range(-DB_MAX, DB_MAX + 1):
            w = ", ".join(f"0x{f32(c):08x}u" for c in section(band, db))
            out.append(f"        {{{w}}},   /* {db:+d} dB */")
        out.append("    },")
    out.append("};")
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    if sys.argv[1:] == ["--check"]:
        sys.exit(0 if OUT.read_text() == text() else f"{OUT}: not what tools/eq_table.py writes")
    OUT.write_text(text())
    print(f"{OUT}: {len(BANDS)} bands, {2 * DB_MAX + 1} gains each")
