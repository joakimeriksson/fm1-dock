#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The integer Mini Pops (modules/minipops/mp_engine.c) against the web engine it was ported from: both
rendered on this computer at 44.1 kHz (tests/minipops_host.c with cc, tests/minipops_ref.mjs with node),
voice by voice (peak frequency, the time to fall 20 dB, the peak level) and the onsets of each rhythm.
  tests/minipops_check.py [PATH/minipops-processor.js]   (default: ~/work/ai-smarthome/audio/software-synth/js)"""
import math
import struct
import subprocess
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
SR = 44100
JS = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.home() / "work/ai-smarthome/audio/software-synth/js/minipops-processor.js"
OUT = SRC / "build" / "host"
OUT.mkdir(parents=True, exist_ok=True)
NAMES = ["BD", "SD1", "SD2", "HH", "CY1", "CY2", "CB", "CO", "LB", "SB", "CL", "QU", "GU", "TA"]
failures = []


def check(case, ok, why):
    print(f"  {'ok  ' if ok else 'FAIL'} {case}: {why}")
    if not ok:
        failures.append(case)


def raw(p):
    d = p.read_bytes()
    return struct.unpack(f"<{len(d) // 2}h", d)


def peak_freq(x, lo, hi):
    """the strongest frequency in the first 100 ms, by a sweep of DFT bins (as the studio's test)"""
    m = int(0.1 * SR)
    best, bf, f = 0.0, 0.0, lo
    while f <= hi:
        w = 2 * math.pi * f / SR
        re = sum(x[i] * math.cos(w * i) for i in range(m))
        im = sum(x[i] * math.sin(w * i) for i in range(m))
        if re * re + im * im > best:
            best, bf = re * re + im * im, f
        f *= 1.02
    return bf


def t20(x):
    """seconds from the loudest 5 ms window to the first one 20 dB under it"""
    w = int(0.005 * SR)
    env = [math.sqrt(sum(v * v for v in x[i:i + w]) / w) for i in range(0, len(x) - w, w)]
    top = max(env)
    m = env.index(top)
    below = next((i for i in range(m, len(env)) if env[i] < top * 0.1), len(env))
    return (below - m) * w / SR


def onsets(x, thresh):
    """sample positions where a 2 ms window's energy first exceeds thresh after a quiet 20 ms"""
    w = int(0.002 * SR)
    out, quiet = [], 0
    for i in range(0, len(x) - w, w):
        e = math.sqrt(sum(v * v for v in x[i:i + w]) / w)
        if e > thresh and quiet >= 10:
            out.append(i)
        quiet = 0 if e > thresh else quiet + 1
    return out


def main():
    exe = OUT / "minipops_host"
    subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter", f"-I{SRC / 'modules/minipops'}",
                    "-o", str(exe), str(SRC / "tests/minipops_host.c"), str(SRC / "modules/minipops/mp_engine.c")], check=True)
    subprocess.run([str(exe), "voices", str(OUT / "mp_c_voices.raw")], check=True)
    subprocess.run(["node", str(SRC / "tests/minipops_ref.mjs"), str(JS), "voices", str(OUT / "mp_js_voices.raw")], check=True)
    c, j = raw(OUT / "mp_c_voices.raw"), raw(OUT / "mp_js_voices.raw")
    check("length", len(c) == len(j) == 14 * SR, f"{len(c)} and {len(j)} samples")
    for v, name in enumerate(NAMES):
        a, b = c[v * SR:(v + 1) * SR], j[v * SR:(v + 1) * SR]
        pa, pb = max(abs(s) for s in a), max(abs(s) for s in b)
        lvl = 20 * math.log10(pa / pb) if pa and pb else 99
        fa, fb = peak_freq(a, 40, 12000), peak_freq(b, 40, 12000)
        ta, tb = t20(a), t20(b)
        ok = abs(lvl) < 1.0 and abs(fa / fb - 1) < 0.05 and (tb < 0.012 or abs(ta / tb - 1) < 0.25)
        check(f"voice {name}", ok, f"peak {pa} / {pb} ({lvl:+.2f} dB), {fa:.0f} / {fb:.0f} Hz, -20 dB in {ta * 1000:.0f} / {tb * 1000:.0f} ms")
    for key, name in ((6, "Rock 1"), (3, "Rumba"), (9, "Swing"), (0, "Waltz")):
        subprocess.run([str(exe), "rhythm", str(key), str(OUT / f"mp_c_r{key}.raw")], check=True)
        subprocess.run(["node", str(SRC / "tests/minipops_ref.mjs"), str(JS), "rhythm", str(key), str(OUT / f"mp_js_r{key}.raw")], check=True)
        a, b = raw(OUT / f"mp_c_r{key}.raw"), raw(OUT / f"mp_js_r{key}.raw")
        oa, ob = onsets(a, 1500), onsets(b, 1500)
        same = len(oa) == len(ob) and all(abs(x - y) <= int(0.002 * SR) for x, y in zip(oa, ob))
        rms_a = math.sqrt(sum(s * s for s in a) / len(a))
        rms_b = math.sqrt(sum(s * s for s in b) / len(b))
        check(f"rhythm {name}", same and abs(20 * math.log10(rms_a / rms_b)) < 1.0,
              f"{len(oa)} / {len(ob)} onsets in 4 s" + ("" if same else f" (first {oa[:6]} vs {ob[:6]})") +
              f", level {20 * math.log10(rms_a / rms_b):+.2f} dB")
    print("all passed" if not failures else f"FAILED: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
