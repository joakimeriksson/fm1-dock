#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The ARP 2600 engine (modules/arp2600/arp_engine.c) on this computer: its pitch, its ladder's cutoff, its
envelopes and its sequencer's clock, each against what the numbers say.
  tests/arp_check.py"""
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


def raw(p):
    d = p.read_bytes()
    return struct.unpack(f"<{len(d) // 2}h", d)


def peak_freq(x, lo, hi, m=None):
    m = m or int(0.2 * SR)
    best, bf, f = 0.0, 0.0, lo
    while f <= hi:
        w = 2 * math.pi * f / SR
        re = sum(x[i] * math.cos(w * i) for i in range(m))
        im = sum(x[i] * math.sin(w * i) for i in range(m))
        if re * re + im * im > best:
            best, bf = re * re + im * im, f
        f *= 1.003
    return bf


def band_db(x, lo, hi):
    """energy in lo..hi Hz over the first 0.4 s, in dB, by a bank of DFT bins a third of an octave apart"""
    m = int(0.4 * SR)
    e, f = 0.0, lo
    while f < hi:
        w = 2 * math.pi * f / SR
        re = sum(x[i] * math.cos(w * i) for i in range(0, m, 2))
        im = sum(x[i] * math.sin(w * i) for i in range(0, m, 2))
        e += re * re + im * im
        f *= 2 ** (1 / 3)
    return 10 * math.log10(e + 1)


def pitch_at(x, t0, lo, hi):
    """the fundamental between lo and hi Hz over 70 ms from t0, by autocorrelation (a lag to the sample)"""
    seg = x[int(t0 * SR):int(t0 * SR) + int(0.07 * SR)]
    best, bl = -1.0, 0
    for lag in range(int(SR / hi), int(SR / lo) + 1):
        c = sum(a * b for a, b in zip(seg, seg[lag:]))
        if c > best: best, bl = c, lag
    return SR / bl


def rms(x, t0, t1):
    seg = x[int(t0 * SR):int(t1 * SR)]
    return math.sqrt(sum(v * v for v in seg) / max(1, len(seg)))


def onsets(x, thresh):
    """attacks: the envelope (|x| over 8 ms, every 2 ms) above thresh and twice what it was 16 ms before, at most
    one in 60 ms (a sub-octave square's own cycles, or two VCOs a few cents apart, move the envelope slowly; an
    attack jumps)"""
    hop, win = int(0.002 * SR), int(0.008 * SR)
    env = [0.0] * 8 + [sum(abs(v) for v in x[i:i + win]) / win for i in range(0, len(x) - win, hop)]   # (silence before)
    out, last = [], -SR
    for k in range(8, len(env)):
        if env[k] > thresh and env[k] > 2 * env[k - 8] and (k - 8) * hop - last > 0.06 * SR:
            out.append((k - 8) * hop)
            last = (k - 8) * hop
    return out


def main():
    exe = OUT / "arp_host"
    subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter", f"-I{SRC / 'modules/arp2600'}",
                    "-o", str(exe), str(SRC / "tests/arp_host.c"), str(SRC / "modules/arp2600/arp_engine.c")], check=True)
    # pitch: TWIN SAWS (two saws 5 cents apart, no vibrato) on A2 and A4
    for note, hz in ((45, 110.0), (69, 440.0)):
        subprocess.run([str(exe), "note", "9", str(note), str(OUT / f"arp_n{note}.raw")], check=True)
        x = raw(OUT / f"arp_n{note}.raw")
        f = peak_freq(x[int(0.3 * SR):], hz * 0.9, hz * 1.1)
        check(f"pitch {note}", abs(f / hz - 1) < 0.006, f"MIDI {note}: {f:.1f} Hz for {hz:.0f} (two VCOs 5 cents apart)")
        check(f"envelope {note}", rms(x, 0.5, 0.9) > 500 and rms(x, 1.9, 2.0) < rms(x, 0.5, 0.9) / 10,
              f"sounds while held (RMS {rms(x, 0.5, 0.9):.0f}), released by 0.9 s after the key ({rms(x, 1.9, 2.0):.0f})")
    # the ladder: noise, the cutoff stepped 10, 30, 50, 70, 90 (notes 31, 53, 75, 97, 119: 49 Hz .. 7.9 kHz): the
    # energy above 2 kHz against below 500 Hz climbs with it, by 20 dB or more over the range
    subprocess.run([str(exe), "sweep", str(OUT / "arp_sweep.raw")], check=True)
    x = raw(OUT / "arp_sweep.raw")
    tilt = []
    for k in range(5):
        seg = x[int((k + 0.1) * SR / 2):int((k + 1) * SR / 2)]
        tilt.append(band_db(seg, 2000, 8000) - band_db(seg, 100, 500))
    check("ladder", tilt[4] - tilt[1] > 20 and all(b >= a - 1 for a, b in zip(tilt[1:], tilt[2:])),
          f"highs against lows as the cutoff climbs: {' '.join(f'{t:+.0f}' for t in tilt)} dB (the first at 49 Hz: the floor)")
    # the sequencer: E MIN ARP (sixteenths at 128 bpm: 117.2 ms) on PLUCK SEQ's pluck, every step a note
    subprocess.run([str(exe), "seq", "1", "0", str(OUT / "arp_seq.raw")], check=True)
    x = raw(OUT / "arp_seq.raw")
    on = onsets(x, 800)
    gaps = [(b - a) / SR for a, b in zip(on, on[1:])]
    step = 60 / 128 / 4
    check("sequencer", len(on) >= 30 and all(abs(g - step) < 0.004 for g in gaps[1:31]),
          f"{len(on)} onsets in 4 s, steps of {step * 1000:.1f} ms (the first gap aside: that attack starts at 0): the first gaps {' '.join(f'{g * 1000:.0f}' for g in gaps[:6])} ms")
    # the rests and ties, on GLASS PLUCK' pluck (one pitch per VCO: no beating): BREATHING has four rests in
    # sixteen (six rests, ten notes); SLOW TIES ties pairs: eight attacks a bar
    subprocess.run([str(exe), "seq", "2", "4", str(OUT / "arp_seq4.raw")], check=True)
    on4 = onsets(raw(OUT / "arp_seq4.raw"), 500)
    subprocess.run([str(exe), "seq", "2", "5", str(OUT / "arp_seq5.raw")], check=True)
    on5 = onsets(raw(OUT / "arp_seq5.raw"), 500)
    bar4, bar5 = 60 / 112 * 4, 60 / 96 * 4
    n4 = len([o for o in on4 if o < (bar4 - 0.02) * SR])
    n5 = len([o for o in on5 if o < (bar5 - 0.02) * SR])
    check("rests and ties", n4 == 10 and n5 == 8, f"BREATHING: {n4} attacks in its bar (10 notes, 6 rests); SLOW TIES: {n5} (8 tied pairs)")
    # the pattern: OCTAVE JAB's links (two bars as written, one a semitone up, one two up) on TWIN SAWS (no glide), by the first
    # note of each bar at 124 (a bar 1.935 s); and the sweep moves the cutoff: the first bar's brightness against the
    # pattern's middle (C MIN PULSE: sweep 60 over six bars, the peak at bar three)
    subprocess.run([str(exe), "seq", "9", "6", str(OUT / "arp_seq6.raw")], check=True)
    x = raw(OUT / "arp_seq6.raw")
    bar = 60 / 124 * 4
    f = [pitch_at(x, k * bar + 0.006, 85, 125) for k in range(4)]
    semis = [12 * math.log2(v / f[0]) for v in f]
    check("pattern", all(abs(s - w) < 0.15 for s, w in zip(semis, (0, 0, 1, 2))),
          f"the first note of bars 1..4: {' '.join(f'{v:.1f}' for v in f)} Hz, {' '.join(f'{s:+.2f}' for s in semis)} semitones (0 0 +1 +2 written)")
    # the explicit performance path (arp_play), on TWIN SAWS (no glide of its own): LEGATO changes pitch without a
    # new attack, SLIDE glides (part-way 5 ms in, there by 200 ms) without one, ACCENT is louder than the same note plain
    subprocess.run([str(exe), "play", "9", str(OUT / "arp_play.raw")], check=True)
    x = raw(OUT / "arp_play.raw")
    on = [o / SR for o in onsets(x, 500)]
    attacks = [round(t * 2) / 2 for t in on]
    check("play attacks", attacks == [0.0, 2.0, 3.0],
          f"attacks at {' '.join(f'{t:.3f}' for t in on)} s: the first note, the plain C3 and the accented one; none at the legato (0.5) or slide (1.0)")
    g3, mid, c4 = pitch_at(x, 0.6, 150, 300), pitch_at(x, 1.005, 150, 300), pitch_at(x, 1.2, 200, 300)
    check("play legato", abs(12 * math.log2(g3 / 130.81) - 7) < 0.15, f"LEGATO to G3: {g3:.1f} Hz (196.0)")
    check("play slide", abs(12 * math.log2(c4 / 130.81) - 12) < 0.15 and 196.0 * 1.03 < mid < 261.6 * 0.97,
          f"SLIDE to C4: {mid:.1f} Hz 5 ms in (between 196 and 262), {c4:.1f} Hz at 200 ms (261.6)")
    plain, accent = rms(x, 2.1, 2.4), rms(x, 3.1, 3.4)
    check("play accent", accent > plain * 1.1, f"ACCENT C3 RMS {accent:.0f} against plain {plain:.0f}")
    print("all passed" if not failures else f"FAILED: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
