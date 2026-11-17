#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Installs over add-ons in the image's room (firmware/src/dock.c room): the update loader and the running firmware's
step 1 (ota.c, firmware/loader/ldr_core.c) run on this computer (tests/ldr_host.c) over the flash chip fm1-live keeps
(--flash), and the result boots in fm1-live.
  tests/install_room_test.py --empty EMPTY.fwsc --standard STANDARD.fwsc [--old OLD.fwsc] [--cuts N]
  - the same package again: nothing above its end is written, the add-on in the room plays after it;
  - Empty -> Standard with the add-on above Standard's end: installed, the add-on stays; a power cut at every Nth
    erase / program of it and the install run again (as the loader does after a cut): it boots and plays, the add-on
    there;
  - Empty -> Standard with an add-on below Standard's end: the loader refuses before it writes (the flash as it was),
    and so does the running firmware's step 1 (nothing staged); both tell the host (OTA_REFUSED_AT);
  - an add-on that an install by another loader wrote over: forgotten at boot, which plays;
  - --old (0.9.0's package) with an add-on in its deploy area and a boot firmware's table entry -> this one (cut at every
    Nth erase / program and run again): the add-on stays, the boot firmware is forgotten, it plays, the room is there.
Needs the packages' sine.dock in build/modules (tools/build.py)."""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "tools"))
sys.path.insert(0, str(SRC / "tests"))
import dock  # noqa: E402
import fm1_install  # noqa: E402
import flashlib as F  # noqa: E402

LDR = SRC / "build" / "ldr_host"
SINE = SRC / "build" / "modules" / "sine.dock"
NOTE = 21
failures = []


def check(case, ok, why):
    print(f"  {'ok  ' if ok else 'FAIL'} {case}: {why}")
    if not ok:
        failures.append(case)


def live(pkg, flash, ms=1500, keys=()):
    """boots pkg's firmware on flash (fm1-live --fwsc --flash: the flash as the file has it) -> (stdout, log)"""
    with tempfile.TemporaryDirectory() as wd:
        env = dict(os.environ, FM1_BENCH_KEYS=",".join(keys or [f"900:{NOTE}"]), FM1_DUMPRAM=str(Path(wd) / "ram.bin"))
        env.pop("FM1_FLASH_CUT", None)
        (Path(wd) / "work").mkdir()
        r = subprocess.run([str(dock.live()), "--fwsc", str(pkg), "--shot", str(ms), "--flash", str(flash)],
                           cwd=wd, capture_output=True, text=True, env=env)
        if r.returncode or "halt" in r.stdout + r.stderr:
            raise SystemExit(f"fm1-live: {r.stdout[-2000:]}{r.stderr[-2000:]}")
        return r.stdout, dock.read_log((Path(wd) / "ram.bin").read_bytes())


def plays(out):
    return "audio peak" in out and int(out.split("audio peak over the last 0.5 s: ")[1].split()[0]) > 1000


def image_of(pkg):
    """the package as the installers serve it (its logical image)"""
    return fm1_install.logical_image(Path(pkg).read_bytes())


def ldr(mode, flash, img, cut=None):
    """ldr_host on the flash file -> (rc, ops, refused)"""
    r = subprocess.run([str(LDR), mode, str(flash), str(img), *([str(cut)] if cut else [])], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"ldr_host: {r.stdout}{r.stderr}")
    rc, ops, refused = (int(x) for x in r.stdout.split()[1::2])
    return rc, ops, refused


def end_of(img_path):
    """a package's end as the loader reads it (its JLFS, 4 KiB up)"""
    img = Path(img_path).read_bytes()
    fl = next(off for typ, off, *_ in fm1_install.package_files(img) if typ == 0)
    return fm1_install.package_end(img[fl:fl + 0x93000])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--empty", type=Path, required=True)
    ap.add_argument("--standard", type=Path, required=True)
    ap.add_argument("--old", type=Path, help="0.9.0's package (any fm1-dock before the room)")
    ap.add_argument("--cuts", type=int, default=25, help="a power cut at every Nth erase / program (default 25)")
    a = ap.parse_args()
    for k in ("empty", "standard", "old"):
        if getattr(a, k):
            setattr(a, k, getattr(a, k).resolve())
    subprocess.run(["cc", "-O2", "-Wall", "-Wno-unused-function", "-Ifirmware/hal", "-Ifirmware/src", "-o", str(LDR),
                    "tests/ldr_host.c"], cwd=SRC, check=True)
    wd = Path(tempfile.mkdtemp(prefix="hb-room-"))
    try:
        sine = SINE.read_bytes()
        add = F.variant(sine, "ROOMSINE", 1)
        imgs = {}
        for k in ("empty", "standard", "old"):
            if getattr(a, k):
                imgs[k] = wd / f"{k}.img"
                imgs[k].write_bytes(image_of(getattr(a, k)))
        e_end, s_end = end_of(imgs["empty"]), end_of(imgs["standard"])
        print(f"ends: Empty {e_end:#x}, Standard {s_end:#x}")

        # the Empty dock with an add-on at the room's top (and one in the plain area)
        base = wd / "empty.bin"
        live(a.empty, base, 1000)
        fl = bytearray(base.read_bytes())
        top = 0x92000
        F.write_table(fl, [F.put(fl, F.variant(sine, "PLAIN1", 1), 0x96000), F.put(fl, add, top)])
        base.write_bytes(fl)
        out, log = live(a.empty, base, 1500, [f"700:m{select('ROOMSINE')}", f"1000:{NOTE}"])
        check("setup", plays(out) and "started" in log and [(n, ok) for n, _, _, ok in F.table(base.read_bytes())] ==
              [("PLAIN1", True), ("ROOMSINE", True)], f"Empty dock, PLAIN1 at 0x96000, ROOMSINE in the room at {top:#x}: picked, it plays")

        # the same package again
        f = wd / "same.bin"
        shutil.copy(base, f)
        before = f.read_bytes()
        rc, ops, ref = ldr("install", f, imgs["empty"])
        after = f.read_bytes()
        out, log = live(a.empty, f, 1500, [f"700:m{select('ROOMSINE')}", f"1000:{NOTE}"])
        check("same_package", rc == 0 and not ref and after[e_end:0x93000] == before[e_end:0x93000] and plays(out) and
              ("ROOMSINE", True) in [(n, ok) for n, _, _, ok in F.table(after)],
              f"rc {rc}, {ops} erases / programs, nothing from {e_end:#x} up changed; ROOMSINE plays after it")

        # Empty -> Standard, the add-on above Standard's end
        f = wd / "to_std.bin"
        shutil.copy(base, f)
        rc, ops, ref = ldr("install", f, imgs["standard"])
        after = f.read_bytes()
        out, log = live(a.standard, f, 1500, [f"700:m{select('ROOMSINE')}", f"1000:{NOTE}"])
        names = [(n, ok) for n, _, _, ok in F.table(after)]
        check("empty_to_standard", rc == 0 and not ref and after[s_end:0x93000] == before[s_end:0x93000] and plays(out)
              and names == [("PLAIN1", True), ("ROOMSINE", True)] and "slot ok 00010" in log and "room: the image ends at" in log,
              f"rc {rc}, {ops} erases / programs below {s_end:#x}; the built-ins and both add-ons check out, ROOMSINE plays")
        total = ops

        def one(k):
            g = wd / f"cut{k}.bin"
            shutil.copy(base, g)
            rc1, _, _ = ldr("install", g, imgs["standard"], k)
            rc2, _, _ = ldr("install", g, imgs["standard"])      # (the SPL runs the loader again; the host resumes)
            out, log = live(a.standard, g, 1500, [f"700:m{select('ROOMSINE')}", f"1000:{NOTE}"])
            t = [(n, ok) for n, _, _, ok in F.table(g.read_bytes())]
            return k, rc1 == 99 and rc2 == 0 and plays(out) and t == [("PLAIN1", True), ("ROOMSINE", True)]
        with ThreadPoolExecutor(4) as ex:
            res = list(ex.map(one, range(1, total + 1, a.cuts)))
        bad = [k for k, ok in res if not ok]
        check("empty_to_standard_cuts", not bad, f"{len(res)} power cuts in {total} erases / programs, each install run "
              "again: it boots, plays ROOMSINE, both add-ons check out" + (f"; failed at {bad}" if bad else ""))

        # Empty -> Standard, an add-on below Standard's end: refused by the loader and by step 1
        f = wd / "covered.bin"
        fl = bytearray(base.read_bytes())
        low = (e_end + 0x3000) & ~0xFFF
        F.write_table(fl, [F.put(fl, F.variant(sine, "PLAIN1", 1), 0x96000), F.put(fl, add, low)])
        f.write_bytes(fl)
        before = f.read_bytes()
        out, log = live(a.empty, f, 1500, [f"700:m{select('ROOMSINE')}", f"1000:{NOTE}"])
        check("covered_setup", plays(out) and ("ROOMSINE", True) in [(n, ok) for n, _, _, ok in F.table(before)],
              f"ROOMSINE at {low:#x}, below Standard's end: it plays on the Empty dock")
        rc, ops, ref = ldr("install", f, imgs["standard"])
        check("loader_refuses", rc == -15 and ops == 0 and ref == 1 and f.read_bytes() == before,
              f"the loader: rc {rc}, {ops} erases / programs, the host told ({ref}), the flash as it was")
        rc, ops, ref = ldr("stage", f, imgs["standard"])
        check("stage_refuses", rc == -15 and ops == 0 and ref == 1 and f.read_bytes() == before,
              f"the running firmware's step 1: rc {rc}, nothing staged ({ops} erases / programs), the host told")
        g = wd / "stage_ok.bin"
        shutil.copy(base, g)
        rc, ops, ref = ldr("stage", g, imgs["standard"])
        rc2, ops2, ref2 = ldr("stage", f, imgs["empty"])
        check("stage_allows", rc == 0 and ops > 0 and not ref and rc2 == 0 and not ref2,
              f"step 1 stages Standard over the room's top ({ops} erases / programs) and Empty over {low:#x} (rc {rc2})")

        # written over by another loader (the whole app area, as 0.9.0's): forgotten at boot
        g = wd / "overwritten.bin"
        fl = bytearray(f.read_bytes())
        img = imgs["standard"].read_bytes()
        flo = next(off for typ, off, *_ in fm1_install.package_files(img) if typ == 0)
        fl[0x4000:0x93000] = img[flo + 0x4000:flo + 0x93000]
        g.write_bytes(fl)
        out, log = live(a.standard, g, 1500)
        check("overwritten_forgotten", plays(out) and "entries forgotten" in log and
              [n for n, *_ in F.table(g.read_bytes())] == ["PLAIN1"],
              "Standard written over ROOMSINE by another loader: the table forgets it at boot, the host plays")

        # 0.9.0 -> this, with an add-on and a boot firmware's entry
        if a.old:
            o = wd / "old.bin"
            live(a.old, o, 1000)
            fl = bytearray(o.read_bytes())
            slots = [F.put(fl, F.variant(sine, "PLAIN1", 1), 0x96000), (F.BOOT, F.xip(0x14F00), 0x40000, 0x12345678)]
            F.write_table(fl, slots)
            o.write_bytes(fl)
            full = wd / "old_full.bin"
            shutil.copy(o, full)
            rc, total, ref = ldr("install", full, imgs["empty"])
            out, log = live(a.empty, full, 1500, [f"700:m{select('PLAIN1')}", f"1000:{NOTE}"])
            check("migrate", rc == 0 and plays(out) and "covered by the image) 00000001" in log and
                  [(n, ok) for n, _, _, ok in F.table(full.read_bytes())] == [("PLAIN1", True)] and
                  f"room: the image ends at {e_end:08x}" in log,
                  f"0.9.0 -> this Empty dock ({total} erases / programs): PLAIN1 plays, the boot firmware's entry forgotten, "
                  f"the room from {e_end:#x}")

            def one_old(k):
                g = wd / f"oldcut{k}.bin"
                shutil.copy(o, g)
                rc1, _, _ = ldr("install", g, imgs["empty"], k)
                rc2, _, _ = ldr("install", g, imgs["empty"])
                out, log = live(a.empty, g, 1500, [f"700:m{select('PLAIN1')}", f"1000:{NOTE}"])
                return k, rc1 == 99 and rc2 == 0 and plays(out) and [n for n, *_ in F.table(g.read_bytes())] == ["PLAIN1"]
            with ThreadPoolExecutor(4) as ex:
                res = list(ex.map(one_old, range(1, total + 1, a.cuts)))
            bad = [k for k, ok in res if not ok]
            check("migrate_cuts", not bad, f"{len(res)} power cuts in {total} erases / programs, each install run again: "
                  "PLAIN1 plays" + (f"; failed at {bad}" if bad else ""))
    finally:
        shutil.rmtree(wd, ignore_errors=True)
    print("all passed" if not failures else f"FAILED: {', '.join(failures)}")
    return 1 if failures else 0


def select(name):
    import hbdeploy
    return hbdeploy.frame(hbdeploy.SELECT, 1, hbdeploy.name24(name)).hex()


if __name__ == "__main__":
    sys.exit(main())
