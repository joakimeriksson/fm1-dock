#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Refresh B3 ORGAN from a newer upstream (github.com/R-Jeb/setBfree-fm1) in one command, and check it.

  modules/b3/refresh.py --tag v0.6.1                 clone that tag (to a temporary directory) and port it
  modules/b3/refresh.py --src ~/src/setBfree-fm1     or port a checkout you already have
      --fwsc build/fm1-dock.fwsc     also: level_test (chords) and an fm1-emu run with a 14-note chord
      --shot                         also: the card's screen picture (docs/media/screen_b3.png), from that run

What it does, in order (REFRESH.md says why, and what to do when a step stops):
  1. take.py            copies the organ and splits dinghy.c into app_init / app_loop (asserts on every patch)
  2. module.cfg         `version =` + 1 (not for the same upstream commit again), the comment line says which
  3. build              tools/hbmod.py modules/b3 -> build/modules/b3.dock (JIELI_TOOLCHAIN, Docker)
  4. docs/modules       b3.dock copied; modules.json: "version", "source" set (description, "save", "alt" are
                        words: read the upstream release notes and edit them by hand)
  5. --fwsc             tests/level_test.py on the new module, then a run in fm1-emu: it must start, play, stay
                        under 90 % of a block with 0 overruns, use the second core, and not clip
It prints what is left for a person: the card's words, the guide and README rows, a run on a real FM-1.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parents[1]
UP = "https://github.com/R-Jeb/setBfree-fm1"


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd))
    r = subprocess.run([str(c) for c in cmd], cwd=SRC, text=True, **kw)
    if r.returncode:
        sys.exit(f"refresh: stopped at: {' '.join(str(c) for c in cmd)}")
    return r


def sub_json(path, key, new):
    """one string field of the "b3" entry, edited in place (the file's layout stays, so the diff stays small)"""
    s = path.read_text()
    i = s.index('"b3": {')
    j = s.index("\n },", i)
    blk = s[i:j]
    m = re.search(r'("%s": )"(?:[^"\\]|\\.)*"' % key, blk)
    assert m, key
    blk = blk[:m.start()] + m.group(1) + json.dumps(new, ensure_ascii=False) + blk[m.end():]
    path.write_text(s[:i] + blk + s[j:])
    json.loads(path.read_text())


def emu_probe(fwsc, flash):
    """the milliseconds the DIN deploy of the module takes in fm1-emu (the module starts after it)"""
    r = subprocess.run([sys.executable, "tools/dock.py", "emu", "--fwsc", fwsc, "--flash", flash, "--deploy",
                        "build/modules/b3.dock", "--at", "800", "--ms", "500", "--keys", "", "--out", flash + ".o"],
                       cwd=SRC, capture_output=True, text=True)
    m = re.search(r"COMMIT in by (\d+)", r.stdout + r.stderr)
    if not m:
        sys.exit("refresh: no COMMIT time from fm1-emu (is FM1_LIVE set, and the fwsc a package?)")
    return int(m.group(1))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--tag", help="an upstream tag (v0.6.1)")
    g.add_argument("--src", help="an upstream checkout")
    ap.add_argument("--fwsc", help="a package to run it in (the page's Standard package, or build/fm1-dock.fwsc)")
    ap.add_argument("--shot", action="store_true", help="make docs/media/screen_b3.png from the emulator run")
    a = ap.parse_args()
    os.environ.setdefault("JIELI_TOOLCHAIN", str(Path.home() / ".jieli/toolchain"))

    tmp = None
    if a.tag:
        tmp = tempfile.TemporaryDirectory()
        up = Path(tmp.name) / "setBfree-fm1"
        run(["git", "clone", "-q", "--branch", a.tag, UP, up])
    else:
        up = Path(a.src).expanduser()
    ver = subprocess.run(["git", "-C", up, "describe", "--tags", "--always"], capture_output=True, text=True).stdout.strip()
    sha = subprocess.run(["git", "-C", up, "rev-parse", "--short=7", "HEAD"], capture_output=True, text=True).stdout.strip()
    print(f"upstream {ver} ({sha})")

    run([sys.executable, HERE / "take.py", up])

    cfg = HERE / "module.cfg"
    t = cfg.read_text()
    same = f", {sha})" in t.splitlines()[0]     # the same upstream commit again: the version stays
    n = int(re.search(r"^version = (\d+)$", t, re.M).group(1)) + (0 if same else 1)
    t = re.sub(r"^version = \d+$", f"version = {n}", t, flags=re.M)
    t = re.sub(r"^# B3 ORGAN: R-Jeb/setBfree-fm1 \([^)]*\)", f"# B3 ORGAN: R-Jeb/setBfree-fm1 ({ver}, {sha})", t, flags=re.M)
    cfg.write_text(t)

    run([sys.executable, "tools/hbmod.py", "modules/b3"])
    run([sys.executable, "tools/hbmod.py", "--info", "build/modules/b3.dock"])
    (SRC / "docs/modules/b3.dock").write_bytes((SRC / "build/modules/b3.dock").read_bytes())
    mj = SRC / "docs/modules/modules.json"
    vn = ver.lstrip("v")
    sub_json(mj, "version", vn)
    sub_json(mj, "source", f"modules/b3: R-Jeb/setBfree-fm1 {sha} (B3 ORGAN {vn}; modules/b3/take.py), fm1-dock")

    if a.fwsc:
        fw = str(Path(a.fwsc).resolve())
        run([sys.executable, "tests/level_test.py", "--fwsc", fw, "build/modules/b3.dock"])
        with tempfile.TemporaryDirectory() as d:
            done = emu_probe(fw, f"{d}/p.bin")
            T = done + 2500
            keys = [f"{T - 500}:e2:0", f"{T - 300}:e3:1"]
            cc = [(70 + i, 0) for i in range(9)] + [(66, 127), (1, 127), (65, 127), (91, 127), (31, 127), (93, 127)]
            keys += [f"{T - 250 + 10 * j}:mb0{c:02x}{v:02x}" for j, (c, v) in enumerate(cc)]
            keys += [f"{T + 20 * j}:m90{k:02x}64" for j, k in enumerate(range(36, 76, 3))]     # 14 notes
            out = f"{d}/run"
            r = subprocess.run([sys.executable, "tools/dock.py", "emu", "--fwsc", fw, "--flash", f"{d}/f.bin", "--deploy",
                                "build/modules/b3.dock", "--at", "800", "--ms", str(T + 2500), "--keys", ",".join(keys),
                                "--out", out], cwd=SRC, capture_output=True, text=True)
            log = r.stdout + r.stderr
            m = re.search(r"worst \d+ \(([\d.]+) %\), (\d+) overruns", log)
            c1 = re.search(r"a job: last \d+ ticks \([\d.]+ %\), worst \d+ \(([\d.]+) %\)", log)
            ok = bool(m) and "b3: B3 ORGAN" in log and int(m.group(2)) == 0 and float(m.group(1)) < 90 and \
                 bool(c1) and float(c1.group(1)) > 0 and "strikes: none" in log
            print(f"emu run: worst block {m.group(1) if m else '?'} %, overruns {m.group(2) if m else '?'}, "
                  f"second core worst {c1.group(1) if c1 else '?'} %  ->  {'OK' if ok else 'FAILED'}")
            if a.shot:                  # its own run: the organ as it starts (Standard B), the controls page woken by a knob
                out = f"{d}/shot"
                subprocess.run([sys.executable, "tools/dock.py", "emu", "--fwsc", fw, "--flash", f"{d}/g.bin", "--deploy",
                                "build/modules/b3.dock", "--at", "800", "--ms", str(T + 1800), "--keys",
                                f"{T - 500}:e2:0,{T - 300}:e3:1,{T + 100}:m904864", "--out", out],
                               cwd=SRC, capture_output=True, text=True)
                try:
                    from PIL import Image
                except ImportError:
                    sys.exit("refresh: --shot needs Pillow (pip install Pillow, or a venv that has it: it is what build.sh needs)")
                if Path(f"{out}/screen.png").exists():      # (dock.py with Pillow: the LCD already cut out)
                    im = Image.open(f"{out}/screen.png").convert("RGB").resize((480, 480), Image.NEAREST)
                else:                                       # (else the whole panel: the LCD at 370, 42, 480 square)
                    pic = next((f"{out}/{n}" for n in ("panel.png", "panel.ppm") if Path(f"{out}/{n}").exists()), None)
                    if not pic:
                        sys.exit(f"refresh: no picture of the panel in {out}: {sorted(os.listdir(out))}")
                    im = Image.open(pic).convert("RGB").crop((370, 42, 370 + 480, 42 + 480))
                im.quantize(colors=32, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).save(
                    SRC / "docs/media/screen_b3.png", optimize=True)
                print("docs/media/screen_b3.png written: look at it")
            if not ok:
                sys.exit(1)

    kb = (SRC / "build/modules/b3.dock").stat().st_size / 1024
    print(f"""
done: {kb:.0f} KiB (4 KiB sectors: the page's size figure; README's row says it).
Left for a person (REFRESH.md): the words ("description", "save", "alt" in modules.json from upstream's release notes),
the guide's B3 card (docs/guide.html) and README's row (size, features), a run on a real FM-1 (room: this is a big
module), then commit.""")


if __name__ == "__main__":
    main()
