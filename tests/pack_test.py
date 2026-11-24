#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""fm1-dock's data packs (hb_module.h "Data packs") in fm1-emu: a pack sent into the DIN jack of the booted package,
and a module that reads it (tests/modules/packuse: it finds TESTPACK by name, logs its length and CRC as read in place
from flash, and plays only while it has the pack).
  ./build.sh && tests/pack_test.py [--fwsc PACKAGE] [--cuts all|N]
  - a pack goes in after its module: the module starts again, finds it, reads every byte right, and plays;
  - after a reboot the pack is in the list (LIST: kind 6, place 0xC0) and the module finds it;
  - a second version of the pack takes the first one's place;
  - DELETE takes the pack out: the module starts again, finds none, and is silent;
  - a pack named as a module already added is refused before the flash is touched (REFUSED), and so is a module
    named as a pack;
  - a power cut at every erase / program of a pack's first deploy: the host always boots and plays, and the table
    has the pack whole or not at all.
The test module and the pack are built here with tools/hbmod.py (JIELI_TOOLCHAIN)."""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "tools"))
import hbdeploy  # noqa: E402
import hbmod  # noqa: E402
sys.path.insert(0, str(SRC / "tests"))
import deploy_test as dt  # noqa: E402
import flashlib  # noqa: E402

NOTE = dt.NOTE
failures = dt.failures
check = dt.check


def built(d):
    r = subprocess.run([sys.executable, str(SRC / "tools" / "hbmod.py"), str(d)], capture_output=True, text=True, cwd=SRC)
    if r.returncode:
        raise SystemExit(f"hbmod {d}: {r.stdout}{r.stderr}")
    return (SRC / "build" / "modules" / f"{Path(d).name}.dock").read_bytes()


def a_pack(name, data, version=1):
    with tempfile.TemporaryDirectory() as wd:
        p = Path(wd) / "d.bin"
        p.write_bytes(data)
        return hbmod.make_pack(name, data, version, Path(wd) / "p.dock").read_bytes()


def select(at, name):
    return f"{at}:m{hbdeploy.frame(hbdeploy.SELECT, 7, hbdeploy.name24(name)).hex()}"


def logged(log, what):
    """the values a 'packuse: <what>' line logged, in order"""
    return [int(ln.split()[-1], 16) for ln in log.splitlines() if f"packuse: {what}" in ln]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cuts", default="all", help="all, or every Nth power-cut point")
    ap.add_argument("--fwsc", type=Path, default=dt.FWSC, help="the package (default build/fm1-dock.fwsc)")
    a = ap.parse_args()
    dt.FWSC = a.fwsc.resolve()
    step = 1 if a.cuts == "all" else int(a.cuts)
    use = built(SRC / "tests" / "modules" / "packuse")
    d1 = bytes([0x40]) + os.urandom(150000 - 1)       # (its first byte: the tone's loudness)
    d2 = bytes([0x80]) + os.urandom(90000 - 1)
    p1, p2 = a_pack("TESTPACK", d1, 1), a_pack("TESTPACK", d2, 2)
    wd = Path(tempfile.mkdtemp(prefix="hb-pack-"))
    try:
        # the module first (no pack: silent), then the pack: the module starts again and reads it
        f1 = wd / "f1.bin"
        e1, t1 = hbdeploy.din_script(use, 600)
        e2, t2 = hbdeploy.din_script(p1, int(t1) + 1500)
        t = int(t2) + 1500
        out, log = dt.live("shot", t + 900, e1 + e2 + [f"{int(t1) + 900}:{NOTE}", f"{int(t1) + 1200}:{NOTE}^",
                                                        f"{t}:{NOTE}"], f1)
        tab = dt.table(f1.read_bytes())
        check("pack_deploy", "packuse: no pack" in log and logged(log, "pack bytes")[-1:] == [len(d1)] and
              logged(log, "pack crc")[-1:] == [zlib.crc32(d1)] and dt.peak(out) > 1000 and
              sorted((n, v, ok) for n, v, _, ok in tab) == [("PACKUSE", 1, True), ("TESTPACK", 1, True)],
              f"no pack: the module is silent; the pack added ({len(d1)} B): it starts again, finds it, every byte "
              f"right (CRC {zlib.crc32(d1):08x}), and plays (peak {dt.peak(out)})")

        ok, log = dt.boots_and_plays(f1)
        out, log = dt.live("shot", 1500, [select(700, "PACKUSE"), f"1100:{NOTE}"], f1)
        check("pack_reboot", "pack ok 0002" in log and logged(log, "pack crc")[-1:] == [zlib.crc32(d1)] and dt.peak(out) > 1000,
              "after a reboot the pack is in the list, and the module finds it and plays")

        # its second version in its place
        f2 = wd / "f2.bin"
        shutil.copy(f1, f2)
        e, tc = hbdeploy.din_script(p2, 1300)
        out, log = dt.live("shot", int(tc) + 4000, [select(700, "PACKUSE")] + e, f2)
        tab = [(n, v, ok) for n, v, _, ok in dt.table(f2.read_bytes())]
        check("pack_replace", sorted(tab) == [("PACKUSE", 1, True), ("TESTPACK", 2, True)] and
              logged(log, "pack crc")[-1:] == [zlib.crc32(d2)],
              f"a second version takes the first one's place; the module reads it ({len(d2)} B)")

        # DELETE: the module finds none, and is silent
        f3 = wd / "f3.bin"
        shutil.copy(f2, f3)
        delete = hbdeploy.frame(hbdeploy.DELETE, 1, hbdeploy.name24("TESTPACK"))
        out, log = dt.live("shot", 3000, [select(700, "PACKUSE"), f"1300:m{delete.hex()}", f"2300:{NOTE}"], f3)
        tab = [(n, v, ok) for n, v, _, ok in dt.table(f3.read_bytes())]
        check("pack_delete", "deploy: deleted" in log and tab == [("PACKUSE", 1, True)] and
              "packuse: no pack" in log.split("deploy: deleted")[-1] and dt.peak(out) < 100,
              "DELETE takes the pack out; the module starts again, finds none, and is silent")

        # names: a pack named as an added module, a module named as an added pack
        f4 = wd / "f4.bin"
        shutil.copy(f1, f4)
        before = f4.read_bytes()
        e, tc = hbdeploy.din_script(a_pack("PACKUSE", d2), 700)
        h, relocs, image = hbmod.parse(use)
        h["name"] = "TESTPACK"
        e2, tc2 = hbdeploy.din_script(hbmod.pack(h, relocs, image), int(tc) + 2500)
        out, log = dt.live("shot", int(tc2) + 2000, e + e2, f4)
        check("pack_names", log.count("deploy failed 0000000b") == 2 and dt.ops(out) == 0 and f4.read_bytes() == before,
              "a pack named as an added module, and a module named as an added pack: both REFUSED, the flash untouched")

        # a new version that fits neither beside the old one nor in its place (the plain area and the room taken by
        # fillers): refused before anything is written, and the old one stays in the table (it was dropped from it
        # before the host knew, until 0.9: the module lost its pack)
        f5 = wd / "f5.bin"
        out, log = dt.live("shot", 1200, [], f5)
        end = [int(ln.split()[-1], 16) for ln in log.splitlines() if "room: the image ends at" in ln]
        flash = bytearray(f5.read_bytes())
        sine = dt.SINE.read_bytes()
        slots = [flashlib.put(flash, use, 0x96000), flashlib.put(flash, p1, 0x97000),
                 flashlib.put(flash, flashlib.variant(sine, "FILLER", 1), 0xBC000, 0x10000)]
        if end:
            slots.append(flashlib.put(flash, flashlib.variant(sine, "ROOMFILL", 1), end[0], 0x93000 - end[0]))
        flashlib.write_table(flash, slots)
        f5.write_bytes(flash)
        before = f5.read_bytes()
        p3 = a_pack("TESTPACK", bytes([0x40]) + os.urandom(200000 - 1), 3)
        e, tc = hbdeploy.din_script(p3, 600)
        out, log = dt.live("shot", int(tc) + 2500, e + [select(int(tc) + 1500, "PACKUSE")], f5)
        tab = sorted((n, v) for n, v, _, ok in dt.table(f5.read_bytes()) if ok)
        check("pack_no_room", "deploy failed 00000004" in log and "the old copy dropped" not in log and
              ("TESTPACK", 1) in tab and f5.read_bytes()[0x93000:0x95000] == before[0x93000:0x95000] and
              logged(log, "pack crc")[-1:] == [zlib.crc32(d1)],
              f"a 200 KB version with room for it nowhere: NO SPACE, the table as it was {tab}, and the module still "
              "reads the old one")

        # power cuts at every erase / program of the pack's first deploy (the module already there)
        f0 = wd / "f0.bin"
        e, tc = hbdeploy.din_script(use, 600)
        dt.live("bench", int(tc) + 1500, e, f0)
        e, tc = hbdeploy.din_script(p1, 600)
        ms = int(tc) + 300
        full = wd / "full.bin"
        shutil.copy(f0, full)
        out, _ = dt.live("bench", ms, e, full)
        n = dt.ops(out)
        allowed = ([("PACKUSE", 1)], [("PACKUSE", 1), ("TESTPACK", 1)])

        def one(k):
            f = wd / f"cut{k}.bin"
            shutil.copy(f0, f)
            dt.live("bench", ms, e, f, cut=k)
            ok, _ = dt.boots_and_plays(f)
            tab = dt.table(f.read_bytes())
            got = sorted((nm, v) for nm, v, _, good in tab if good)
            return k, ok and all(x[3] for x in tab) and got in allowed, got
        with ThreadPoolExecutor(4) as ex:
            res = list(ex.map(one, range(1, n + 1, step)))
        fails = [k for k, ok, _ in res if not ok]
        seen = sorted({str(g) for _, _, g in res})
        check("pack_cuts", not fails and n > 0, f"{len(res)} power cuts in {n} erases / programs: the host boots and "
              f"plays every time; the table afterwards: {' or '.join(seen)}" + (f"; failed at {fails}" if fails else ""))
    finally:
        shutil.rmtree(wd, ignore_errors=True)
    print("all passed" if not failures else f"FAILED: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
