#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The Eminent 310 engine (modules/eminent/em_engine.c) on this computer: the string formant, the ensemble's width,
the phaser's sweep, the envelope's times.
  tests/eminent_check.py"""
import math
import struct
import subprocess
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
SR = 44100
OUT = SRC / "build" / "host"
OUT.mkdir(parents=True, exist_ok=True)
failures = []


def check(case, ok, why):
    print(f"  {'ok  ' if ok else 'FAIL'} {case}: {why}")
    if not ok:
        failures.append(case)


def render(name, *args):
    p = OUT / f"em_{name}.raw"
    subprocess.run([str(OUT / "eminent_host"), *map(str, args), str(p)], check=True)
    d = p.read_bytes()
    x = struct.unpack(f"<{len(d) // 2}h", d)
    return x[0::2], x[1::2]


def band_db(x, t0, t1, lo, hi):
    seg = x[int(t0 * SR):int(t1 * SR)]
    m = len(seg)
    e, f = 0.0, lo
    while f < hi:
        w = 2 * math.pi * f / SR
        re = sum(seg[i] * math.cos(w * i) for i in range(0, m, 2))
        im = sum(seg[i] * math.sin(w * i) for i in range(0, m, 2))
        e += re * re + im * im
        f *= 2 ** (1 / 6)
    return 10 * math.log10(e + 1)


def tone_db(x, t0, t1, f):
    """the level at one frequency"""
    seg = x[int(t0 * SR):int(t1 * SR)]
    w = 2 * math.pi * f / SR
    re = sum(v * math.cos(w * i) for i, v in enumerate(seg))
    im = sum(v * math.sin(w * i) for i, v in enumerate(seg))
    return 10 * math.log10(re * re + im * im + 1)


def harmonics_db(x, t0, t1, f0, lo, hi):
    """the energy of f0's harmonics inside lo .. hi (band_db's sixth-octave points pass between them)"""
    return 10 * math.log10(sum(10 ** (tone_db(x, t0, t1, f0 * k) / 10) for k in range(1, int(hi / f0) + 1) if lo <= f0 * k < hi))


def rms(x, t0, t1):
    seg = x[int(t0 * SR):int(t1 * SR)]
    return math.sqrt(sum(v * v for v in seg) / max(1, len(seg)))


def main():
    subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter", f"-I{SRC / 'modules/eminent'}", "-o",
                    str(OUT / "eminent_host"), str(SRC / "tests/eminent_host.c"), str(SRC / "modules/eminent/em_engine.c")], check=True)
    # the formant: A3 dry, 8' and 4' saws; bare, that mix has 7.9 dB less at 1.5 .. 3 kHz than at 300 .. 600 Hz (the
    # 8' harmonics 7 .. 13 with the 4' ones among them, against 440 Hz from both); the string filter lifts the upper
    # band by 4 dB or more
    L, R = render("dry", 57, 0, 0, 0, 45, 55)
    # the tuning first: key 57 is A3 at 220 Hz, key 59 the B over it (the note table was once read a row up: every
    # key a semitone sharp and a B the C eleven under it; the checks below then measured an A#3 and passed by leakage)
    a3 = tone_db(L, 1.0, 1.4, 220.0) - max(tone_db(L, 1.0, 1.4, 233.08), tone_db(L, 1.0, 1.4, 207.65))
    Lb, _ = render("b3", 59, 0, 0, 0, 45, 55)
    b3 = tone_db(Lb, 1.0, 1.4, 246.94) - max(tone_db(Lb, 1.0, 1.4, 130.81), tone_db(Lb, 1.0, 1.4, 261.63))
    check("tuning", a3 > 20 and b3 > 20, f"key 57 at 220 Hz {a3:+.0f} dB over its neighbours, key 59 at 247 Hz {b3:+.0f} dB over the Cs")
    tilt = harmonics_db(L, 1.0, 1.4, 220.0, 1500, 3000) - harmonics_db(L, 1.0, 1.4, 220.0, 300, 600)
    check("formant", tilt > -3.9, f"A3's 1.5 .. 3 kHz against 300 .. 600 Hz: {tilt:+.1f} dB (the bare saws: -7.9)")
    c = sum(a * b for a, b in zip(L, R)) / math.sqrt(sum(a * a for a in L) * sum(b * b for b in R))
    L2, R2 = render("ens", 57, 70, 0, 0, 45, 55)
    c2 = sum(a * b for a, b in zip(L2, R2)) / math.sqrt(sum(a * a for a in L2) * sum(b * b for b in R2))
    check("ensemble", c > 0.99 and c2 < 0.5 and rms(L2, 1.0, 1.9) > rms(L, 1.0, 1.9) * 0.4,
          f"left against right: {c:.3f} without the ensemble, {c2:.3f} with it (three lines a third of a cycle apart)")
    # the phaser at 60 (0.38 Hz): A3's fifth harmonic (1100 Hz) dips by 8 dB or more as a notch passes it
    L3, _ = render("ph", 57, 0, 60, 1, 0, 55)
    lv = [tone_db(L3, t, t + 0.1, 1100.0) for t in (0.2 + 0.1 * k for k in range(16))]
    check("phaser", max(lv) - min(lv) > 8, f"1100 Hz over 1.6 s: {min(lv):.0f} .. {max(lv):.0f} dB, a {max(lv) - min(lv):.0f} dB notch passing")
    # the envelope: attack 0 is 20 ms, 100 is 1.5 s; release 0 is 50 ms, 100 is 3 s
    La, _ = render("fast", 57, 0, 0, 0, 0, 0)
    Ls, _ = render("slow", 57, 0, 0, 0, 100, 100)
    fast_up = rms(La, 0.04, 0.08) > 0.8 * rms(La, 1.0, 1.5)
    slow_up = rms(Ls, 0.4, 0.5) < 0.5 * rms(Ls, 1.8, 2.0)
    fast_down = rms(La, 2.3, 2.4) < 0.05 * rms(La, 1.0, 1.5)
    slow_down = rms(Ls, 3.0, 3.1) > 0.5 * rms(Ls, 1.8, 2.0)
    check("envelope", fast_up and slow_up and fast_down and slow_down,
          f"attack 0 up by 60 ms ({fast_up}), 100 still rising at 0.5 s ({slow_up}); release 0 gone in 0.3 s ({fast_down}), 100 half there a second on ({slow_down})")
    # no clicks: a chord held eight seconds with the phaser at 25 (a cycle 8.5 s) never jumps a third of full scale
    # between two samples (the phaser's sweep table once read past its end at the triangle's peak)
    Lc, _ = render("chord", "chord", 70, 25)
    jumps = [i for i in range(1, len(Lc)) if abs(Lc[i] - Lc[i - 1]) > 12000]
    check("no clicks", not jumps, f"{len(jumps)} jumps over a third of full scale in 8 s of a chord with the phaser")
    print("all passed" if not failures else f"FAILED: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
