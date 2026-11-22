#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Code written where code ran (#87), on a real FM-1 over USB: it deploys and deletes a test module, CTEST
(tests/modules/ctest), and reads the log. Not in fm1-emu: that keeps decoded instructions by address, over a
flash write and over a reset.
  uv run --with python-rtmidi tests/cache_hw_test.py [ROUNDS]        (JIELI_TOOLCHAIN: it builds CTEST twice)
Its two builds have the same layout and other numbers in their code. They are deployed in turn, each after a DELETE,
so to the same flash; each logs at its start what its code adds up to. A value that is not the build's own: old code
ran. CTEST is deleted and the synth that played is picked again at the end, also when a round fails."""
import sys
import time
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "tools"))
import hbdeploy as hd  # noqa: E402
import hbmod  # noqa: E402

D = SRC / "tests" / "modules" / "ctest"


def build(v):
    (D / "module.cfg").write_text(f"name = CTEST\nversion = {v}\ncflags = -DVAL={v}u\n")
    return hbmod.build_module(D).read_bytes()


def want(v):
    return f"{4032 + 64000 * v:08x}"


def drop(p):
    if any(m["name"] == "CTEST" for m in hd.modules(p)):
        p.ask(hd.DELETE, hd.name24("CTEST"), timeout=10.0)
        time.sleep(0.5)


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 6
    blobs = {v: build(v) for v in (2, 1)}
    p = hd.Port()
    was = hd.modules(p)[hd.hello(p)["cur"]]["name"]
    bad = places = 0
    last = None
    try:
        for n in range(rounds):
            v = 1 + n % 2
            drop(p)
            hd.deploy(p, blobs[v], say=lambda *_: None)
            time.sleep(1.5)
            log = hd.log_text(p)[0].splitlines()
            get = lambda w: ([ln.split()[-1] for ln in log if w in ln] or ["-"])[-1]
            at, b, r = get("deploy: written at"), get("ctest: built"), get("ctest: ran")
            ok = b == r == want(v)
            bad += not ok
            places += at == last
            last = at
            print(f"  {'ok  ' if ok else 'FAIL'} build {v} at {at}: its code says {b} / {r}, want {want(v)}")
    finally:
        drop(p)
        print("the synth before:", was, hd.status(p.ask(hd.SELECT, hd.name24(was), timeout=10.0)),
              "| last crash pc", hex(hd.hello(p)["crash_pc"]))
    if not places:
        print("no two builds at the same address: nothing was tested")
    print("all passed" if not bad and places else "FAILED")
    return 1 if bad or not places else 0


if __name__ == "__main__":
    sys.exit(main())
