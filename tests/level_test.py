#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Big chords without distortion: each add-on in fm1-emu (fm1-live, headless), deployed into a host package as the
page does it, plays a note and then an eight-note chord, both at velocity 127, on some of its patches; the chord
must not come out clipped.
  tests/level_test.py --fwsc build/fm1-dock.fwsc [docs/modules/fm6.dock ...]     (no .dock: docs/modules/*.dock)
A clip is a flat top: a stretch where the wave sits at one value for several samples in a row near its peak (a
clamp, in the module or at the host's 16 bits). A soft limiter or a tanh keeps the wave moving, so a port that
follows its original's mix stays clean; one that cuts at a fixed level does not (0.9: FM6 cut at 20596, FMINI-1
at 32767, heard on TINE EP and PIPE ORGAN chords). A patch whose own wave is flat (a square) is flat in the
single note too: the chord fails only when it is flatter than the note by a margin.
--json FILE writes the measurements. Exit 1 when a chord clips."""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import wave
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "tools"))
import hbmod  # noqa: E402

# patches (MIDI program numbers) played per module: the first few and the loud ones that showed it; others: 0..3
PROGRAMS = {
    "FM6": [1, 2, 7, 10, 15, 0],                # TINE EP, GLASS BELL, ORGAN, BRASS SECT, STRINGS (F16), the first
    "FMINI-1": [0, 1, 2, 71, 3],                # ... PIPE ORGAN
    "KEYS": [0, 1, 2],                          # clavinet, electric piano, piano
    "PROPHET": [0, 1, 2, 3, 4],
    "S1VAVE": [0, 3, 4, 6, 11, 12],              # POLY SAW, PWM PAD, BRASS, CHORD STAB, STRINGS, SYNTH KEYS
}
# patches picked with the PRESETS encoder (a module that takes no program change): their places in its list
ENC_PRESETS = {
    "VECTOR37": [0, 2, 4, 8, 9, 16, 17, 18, 19],   # NEBULA PAD, GLASS BELLS, DEEP DRONE, WAVE ORBIT, PPG GLASS,
}                                               # WARM STRINGS, PWM PAD, CHOIR AAH, DRAWBAR ORGAN
# data packs an add-on needs (hb_module.h "Data packs"): deployed after it, as the page does (the module first: the
# bigger one takes the plain area), from beside its .dock or build/modules/
PACKS = {"VECTOR37": "v37-wt.dock"}
PANEL = {"ARTIHIVE": "8"}                       # played on the FM-1's own keys (its MIDI notes gate fixed voices),
                                                # after this key (ARTIHIVE: POLY)
SKIP = {"NIAGARA": "two drones, from scene keys: no chords to play"}
PANEL_CHORD = (14, 16, 18, 21, 23, 25, 28, 30)  # eight of the FM-1's keys (tests/emu_test.py's numbers)
CHORD = (36, 48, 55, 60, 64, 67, 71, 76)        # C2 C3 G3 C4 E4 G4 B4 E5
DEPLOY_MS = 0.42                                # fm1-live's deploy (from 1000 ms) per byte of the add-on: 43 KiB ~18.5 s
STEP = 4000                                     # a patch's turn: the note 300..1300, the chord 1800..3300 ms
RUN_MIN = 3                                     # samples at one value in a row: a flat top
NEAR = 0.6                                      # ... at or above this share of the window's peak
LOUD = 2000                                     # ... and this level: a quiet wave repeats values by itself
LIMIT = 0.002                                   # the chord's flat share allowed over the note's (0.2 %)


def script(progs, t0, panel=None, enc=False):
    k, at = [], 0
    for i, p in enumerate(progs):
        b = t0 + i * STEP
        if enc:                                 # the PRESETS encoder to place p (6: PRESETS)
            if p != at:
                k.append(f"{b - 200}:e6:{p - at}")
            at = p
            k += [f"{b + 300}:m903c7f", f"{b + 1300}:m803c00"]
            k += [f"{b + 1800}:m90{n:02x}7f" for n in CHORD] + [f"{b + 3300}:m80{n:02x}00" for n in CHORD]
            continue
        if panel:
            k += [f"{b - 400}:{panel}", f"{b - 350}:{panel}^"] if i == 0 else []
            k += [f"{b + 300}:21", f"{b + 1300}:21^"]
            k += [f"{b + 1800}:{n}" for n in PANEL_CHORD] + [f"{b + 3300}:{n}^" for n in PANEL_CHORD]
            continue
        k.append(f"{b}:mc0{p:02x}")
        k += [f"{b + 300}:m903c7f", f"{b + 1300}:m803c00"]
        k += [f"{b + 1800}:m90{n:02x}7f" for n in CHORD] + [f"{b + 3300}:m80{n:02x}00" for n in CHORD]
    return ",".join(k), t0 + len(progs) * STEP + 300


def flat_share(x):
    """the share of samples in runs of RUN_MIN or more at one value, at NEAR of the peak and LOUD or above"""
    pk = max((abs(v) for v in x), default=0)
    if pk < 200:
        return 0.0, pk
    lo, n, i = max(NEAR * pk, LOUD), 0, 0
    while i < len(x):
        j = i
        while j + 1 < len(x) and x[j + 1] == x[i]:
            j += 1
        if j - i + 1 >= RUN_MIN and abs(x[i]) >= lo:
            n += j - i + 1
        i = j + 1
    return n / len(x), pk


def window(s, sr, t0, t1):
    """the left channel between t0 and t1 ms of the run (the WAV starts with the audio, ~200 ms after boot)"""
    a, b = int((t0 - 200) * sr / 1000), int((t1 - 200) * sr / 1000)
    return s[2 * a:2 * b:2]


def rms(x):
    return (sum(v * v for v in x) / max(1, len(x))) ** 0.5


def measure(fwsc, dock, out):
    raw = Path(dock).read_bytes()
    name = hbmod.parse(raw)[0]["name"]
    if name in SKIP:
        return name, {"skipped": SKIP[name]}
    enc = name in ENC_PRESETS
    progs = [0] if name in PANEL else ENC_PRESETS[name] if enc else PROGRAMS.get(name, [0, 1, 2, 3])
    deploy = [str(dock)]
    pre = []
    if name in PACKS:                           # its pack after it (both by their DIN scripts: one run)
        import hbdeploy
        pk = next((d / PACKS[name] for d in (Path(dock).parent, SRC / "build" / "modules") if (d / PACKS[name]).exists()), None)
        if not pk:
            return name, {"error": f"its data pack {PACKS[name]} is not built (tools/hbmod.py --pack)"}
        e1, t1 = hbdeploy.din_script(raw, 1000)
        e2, t2 = hbdeploy.din_script(pk.read_bytes(), int(t1) + 4500)
        pre, deploy = e1 + e2, []
        t0 = int(t2) + 4500                     # (the module starts again with its pack: its boot's 3 s)
    else:
        t0 = 1000 + int(len(raw) * DEPLOY_MS) + 3000
    keys, ms = script(progs, t0, PANEL.get(name), enc)
    keys = ",".join(pre + [keys]) if pre else keys
    o = out / Path(dock).stem
    r = subprocess.run([sys.executable, str(SRC / "tools" / "dock.py"), "emu", "--fwsc", str(fwsc),
                        *(["--deploy", deploy[0]] if deploy else []), "--ms", str(ms), "--keys", keys, "--out", str(o)],
                       capture_output=True, text=True)
    log = (o / "log.txt").read_text(errors="replace") if (o / "log.txt").exists() else ""
    if r.returncode or not (o / "audio.wav").exists():
        return name, {"error": (r.stderr or r.stdout)[-400:]}
    w = wave.open(str(o / "audio.wav"))
    sr, d = w.getframerate(), w.readframes(w.getnframes())
    s = struct.unpack(f"<{len(d) // 2}h", d)
    done = list(re.finditer(r"^(\d+) deploy: done", log, re.M))
    done = done[-1] if done else None              # (with a pack: its deploy, the last)
    res = {"faults": log.count("FAULT"), "patches": [],
           "started": bool(done) and int(done.group(1)) < t0 - 300 and
           re.search(rf"^\d+ started \w+\n\d+ {re.escape(name)}$", log[done.start():], re.M) is not None}
    for i, p in enumerate(progs):
        b = t0 + i * STEP
        note, chord = window(s, sr, b + 450, b + 1300), window(s, sr, b + 1950, b + 3300)
        fn, pn = flat_share(note)
        fc, pc = flat_share(chord)
        clip = sum(1 for v in chord if abs(v) >= 32767)
        res["patches"].append({"program": p, "note_peak": pn, "chord_peak": pc, "note_rms": round(rms(note)),
                               "chord_rms": round(rms(chord)), "note_flat": round(fn, 5), "chord_flat": round(fc, 5),
                               "clipped_16bit": clip, "ok": clip == 0 and fc <= fn + LIMIT and pc >= 200})
    return name, res


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("docks", nargs="*")
    ap.add_argument("--fwsc", required=True, help="the host package the add-ons are deployed into")
    ap.add_argument("--out", default=str(SRC / "build" / "level"))
    ap.add_argument("--json")
    ap.add_argument("-j", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    a = ap.parse_args()
    docks = a.docks or sorted(str(p) for p in (SRC / "docs" / "modules").glob("*.dock"))
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(a.j) as ex:
        results = list(ex.map(lambda d: (d, *measure(a.fwsc, d, out)), docks))
    bad, report = 0, {}
    for dock, name, res in results:
        report[Path(dock).name] = {"module": name, **res}
        if "skipped" in res:
            print(f"--   {name} ({Path(dock).name}): skipped, {res['skipped']}")
            continue
        if "error" in res:
            print(f"FAIL {name} ({Path(dock).name}): the run failed: {res['error']}")
            bad += 1
            continue
        ok_run = res["faults"] == 0 and res["started"]
        if not ok_run:
            bad += 1
        print(f"{'ok  ' if ok_run and all(p['ok'] for p in res['patches']) else 'FAIL'} {name} "
              f"({Path(dock).name}){'' if ok_run else ': did not start, or faulted'}")
        for p in res["patches"]:
            bad += not p["ok"]
            clip = f" | {p['clipped_16bit']} at 16-bit full scale" if p["clipped_16bit"] else ""
            print(f"   {'  ' if p['ok'] else '!!'} program {p['program']:3}: note peak {p['note_peak']:5} flat "
                  f"{100 * p['note_flat']:5.2f} % | chord peak {p['chord_peak']:5} rms {p['chord_rms']:5} flat "
                  f"{100 * p['chord_flat']:5.2f} %{clip}"
                  f"{' | silent' if p['chord_peak'] < 200 else ''}")
    if a.json:
        Path(a.json).write_text(json.dumps(report, indent=1))
    print(f"{bad} problem(s)" if bad else "every chord clean")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
