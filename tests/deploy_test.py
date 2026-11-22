#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""fm1-dock's deploys in fm1-emu: modules sent into the DIN jack of the booted package (fm1-live
--fwsc, --flash: the flash chip kept between runs, FM1_FLASH_CUT: a power cut half way through
the Nth erase / program).
  ./build.sh && tests/deploy_test.py [--cuts all|N]
  - a new module goes in, runs, is there after a reboot, and goes again with DELETE;
  - a damaged transfer is refused before the flash is touched;
  - a power cut at every erase / program of a first deploy and of a replacing one: the host always
    boots and plays, the table lists only modules that check out, and the module is the old or the new;
  - a module that renders slower than real time is stopped, and the sine plays;
  - a module's bank (CS-1's user presets) is there after a reboot; with no bank free its save is refused and logged.
  - the image's room (a package that leaves room below 0x93000; --fwsc build/... of the Empty dock has the most): with
    the plain area full, a module goes into the room, from its top, encrypted; it runs and is there after a reboot,
    and a power cut at every erase / program of a first deploy and of a replacing one there leaves a host that boots
    and plays, with the old module or the new.
  tests/deploy_test.py [--fwsc PACKAGE] [--cuts all|N]
The test module (tests/modules/slow) is built here with tools/hbmod.py (JIELI_TOOLCHAIN)."""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "tools"))
import dock  # noqa: E402
import hbdeploy  # noqa: E402
import hbmod  # noqa: E402
sys.path.insert(0, str(SRC / "tests"))
import flashlib  # noqa: E402

FWSC = SRC / "build" / "fm1-dock.fwsc"
SINE = SRC / "build" / "modules" / "sine.dock"
DT = (0x93000, 0x94000)
SETTINGS = 0x95000                              # dock.c SETTINGS_AT
NOTE = 21                                       # C4's key (14 + 7)
failures = []


def check(case, ok, why):
    print(f"  {'ok  ' if ok else 'FAIL'} {case}: {why}")
    if not ok:
        failures.append(case)


def variant(name, version):
    """the sine module under another name / version (its CRC made again)"""
    h, relocs, image = hbmod.parse(SINE.read_bytes())
    h["name"], h["version"] = name, version
    return hbmod.pack(h, relocs, image)


def live(mode, ms, keys, flash, cut=None):
    """one fm1-live run of the package; flash: the file it starts from and leaves its flash in.
    shot: (stdout, the host's log); bench: (stdout, None)"""
    with tempfile.TemporaryDirectory() as wd:
        (Path(wd) / "work").mkdir()
        env = dict(os.environ, FM1_BENCH_KEYS=",".join(keys))
        env.pop("FM1_FLASH_CUT", None)
        if cut is not None:
            env["FM1_FLASH_CUT"] = str(cut)
        ram = Path(wd) / "ram.bin"
        if mode == "shot":
            env["FM1_DUMPRAM"] = str(ram)
        r = subprocess.run([str(dock.live()), "--fwsc", str(FWSC), f"--{mode}", str(ms), "--flash", str(flash)],
                           cwd=wd, capture_output=True, text=True, env=env)
        if r.returncode or "halt" in r.stdout + r.stderr:
            raise SystemExit(f"fm1-live: {r.stdout[-2000:]}{r.stderr[-2000:]}")
        return r.stdout, dock.read_log(ram.read_bytes()) if mode == "shot" else None


def ops(out):
    for ln in out.splitlines():
        if ln.startswith("flash: "):
            return int(ln.split()[1])
    return 0


def peak(out):
    return int(out.split("audio peak over the last 0.5 s: ")[1].split()[0]) if "audio peak" in out else 0


def table(flash):
    """the deployed modules as the flash holds them: [(name, version, flash offset, checks out)] (flashlib.table: the
    room's decrypted, as fm1-dock reads it)"""
    return flashlib.table(flash)


def boots_and_plays(flash):
    out, log = live("shot", 1500, [f"900:{NOTE}"], flash)
    refused = [ln for ln in log.splitlines() if "slot refused 0002" in ln]
    return "boot" in log and peak(out) > 1000 and not refused, log


def deploy_keys(blob, at=600):
    keys, done = hbdeploy.din_script(blob, at)
    return keys, done


def main():
    global FWSC
    ap = argparse.ArgumentParser()
    ap.add_argument("--cuts", default="all", help="all, or every Nth power-cut point")
    ap.add_argument("--fwsc", type=Path, default=FWSC, help="the package (default build/fm1-dock.fwsc)")
    a = ap.parse_args()
    FWSC = a.fwsc.resolve()
    step = 1 if a.cuts == "all" else int(a.cuts)
    wd = Path(tempfile.mkdtemp(prefix="hb-deploy-"))
    try:
        v1, v2 = variant("TESTSINE", 1), variant("TESTSINE", 2)

        # a new module: in, running, there after a reboot, and gone with DELETE
        f1 = wd / "f1.bin"
        keys, done = deploy_keys(v1)
        out, log = live("shot", int(done) + 900, keys + [f"{int(done) + 300}:{NOTE}"], f1)
        t = table(f1.read_bytes())
        check("deploy", "deploy: done" in log and "slot ok 0002" in log and peak(out) > 1000 and
              [(n, v, ok) for n, v, _, ok in t] == [("TESTSINE", 1, True)],
              f"a new module over the DIN jack: {ops(out)} erases / programs, in flash at {t[0][2]:#x} if any, then playing")
        ok, log = boots_and_plays(f1)
        check("deploy_reboot", ok and "TESTSINE" in log, "after a reboot it is in the list")
        f2 = wd / "f2.bin"
        shutil.copy(f1, f2)
        delete = hbdeploy.frame(hbdeploy.DELETE, 1, hbdeploy.name24("TESTSINE"))
        out, log = live("shot", 1200, [f"700:m{delete.hex()}"], f2)
        check("delete", "deploy: deleted" in log and table(f2.read_bytes()) == [], "DELETE takes it out of the table")

        # a damaged transfer: refused before any erase / program
        f3 = wd / "f3.bin"
        frames = hbdeploy.deploy_frames(v1)
        cmd, payload = frames[3]
        frames[3] = (cmd, payload[:20] + bytes([payload[20] ^ 1]) + payload[21:])   # (its own frame CRC made anew)
        keys, t = [], 600
        for i, (c, p) in enumerate(frames):
            f = hbdeploy.frame(c, i, p)
            t += 400 if c == hbdeploy.COMMIT else 0
            keys.append(f"{int(t)}:m{f.hex()}")
            t += len(f) * hbdeploy.BYTE_MS + (400 if c == hbdeploy.BEGIN else 0)
        out, log = live("shot", int(t) + 2500, keys + [f"{int(t) + 2000}:{NOTE}"], f3)
        check("bad_transfer", "deploy failed 00000001" in log and ops(out) == 0 and peak(out) > 1000,
              f"a chunk changed on the way: BAD CRC at COMMIT, {ops(out)} erases / programs, the sine plays on")

        # power cuts at every erase / program
        def cuts(name, base, blob, allowed):
            keys, done = deploy_keys(blob)
            ms = int(done) + 300
            full = wd / f"{name}_full.bin"
            if base:
                shutil.copy(base, full)
            out, _ = live("bench", ms, keys, full)
            n = ops(out)
            new = sorted((nm, v) for nm, v, _, good in table(full.read_bytes()) if good)
            points = list(range(1, n + 1, step))

            def one(k):
                f = wd / f"{name}_cut{k}.bin"
                if base:
                    shutil.copy(base, f)
                live("bench", ms, keys, f, cut=k)
                ok, log = boots_and_plays(f)
                got = sorted((nm, v) for nm, v, _, good in table(f.read_bytes()) if good)
                bad = [x for x in table(f.read_bytes()) if not x[3]]
                return k, ok and not bad and got in allowed, got
            with ThreadPoolExecutor(4) as ex:
                res = list(ex.map(one, points))
            seen = sorted({str(g) for _, _, g in res})
            fails = [k for k, ok, _ in res if not ok]
            check(name, not fails and new == allowed[1], f"{len(points)} power cuts in {n} erases / programs: the host "
                  f"boots and plays every time; the table afterwards: {' or '.join(seen)} (uncut: {new})" +
                  (f"; failed at {fails}" if fails else ""))
            return full

        cuts("cut_first", None, v1, ([], [("TESTSINE", 1)]))
        cuts("cut_replace", f1, v2, ([("TESTSINE", 1)], [("TESTSINE", 2)]))

        # the image's room: the plain area taken (one entry as large as it), a module goes into the room
        fr = wd / "room0.bin"
        out, log = live("shot", 1200, [], fr)
        end = [int(ln.split()[-1], 16) for ln in log.splitlines() if "room: the image ends at" in ln]
        if not end:
            check("room", False, "no room in this package's image (the log has no 'room: the image ends at')")
        else:
            flash = bytearray(fr.read_bytes())
            flashlib.write_table(flash, [flashlib.put(flash, variant("AREA1", 1), 0x96000, 0xE0000 - 0x96000)])
            fr.write_bytes(flash)
            f6 = wd / "room1.bin"
            shutil.copy(fr, f6)
            keys, done = deploy_keys(v1)
            out, log = live("shot", int(done) + 900, keys + [f"{int(done) + 300}:{NOTE}"], f6)
            t = table(f6.read_bytes())
            at = [o for n, _, o, ok in t if n == "TESTSINE" and ok]
            check("room_deploy", "deploy: done" in log and peak(out) > 1000 and at == [0x92000] and
                  [(n, ok) for n, _, _, ok in t] == [("AREA1", True), ("TESTSINE", True)] and
                  bytes(f6.read_bytes()[0x92000:0x92004]) != bytes(flashlib.read(f6.read_bytes(), 0x92000, 4)),
                  f"the image ends at {end[0]:#x}; with the plain area full, a module goes into the room at "
                  f"{at[0] if at else 0:#x} (its top), stored encrypted, then playing")
            ok, log = boots_and_plays(f6)
            check("room_reboot", ok and "TESTSINE" in log and "slot ok 0002" in log, "after a reboot it is in the list, "
                  "read through the SFC")
            cuts("room_cut_first", fr, v1, ([("AREA1", 1)], [("AREA1", 1), ("TESTSINE", 1)]))
            cuts("room_cut_replace", f6, v2, ([("AREA1", 1), ("TESTSINE", 1)], [("AREA1", 1), ("TESTSINE", 2)]))

        # too slow: stopped, the sine plays
        r = subprocess.run([sys.executable, str(SRC / "tools" / "hbmod.py"), str(SRC / "tests" / "modules" / "slow")],
                           capture_output=True, text=True, cwd=SRC)
        if r.returncode:
            raise SystemExit(f"hbmod tests/modules/slow: {r.stdout}{r.stderr}")
        f5 = wd / "f5.bin"
        keys, done = deploy_keys((SRC / "build" / "modules" / "slow.dock").read_bytes())
        t = int(done) + 300
        out, log = live("shot", t + 3000, keys + [f"{t}:{NOTE}", f"{t + 1800}:{NOTE}^", f"{t + 2300}:{NOTE + 2}"], f5)
        stop = [int(ln.split()[0]) for ln in log.splitlines() if "too slow: stopped" in ln]
        check("too_slow", "slow: ready" in log and stop and stop[0] - t < 1200 and "started 00000000" in log.split("too slow")[-1]
              and peak(out) > 1000, f"a module 1.5 blocks slow is stopped {stop[0] - t if stop else '-'} ms after it got slow; "
              "the sine plays the next key")

        # a module's bank: CS-1's user presets, there after a reboot
        img = (SRC / "build" / "fm1-dock.bin").read_bytes()
        names = []
        for i in range(32):
            magic, at, size, crc = struct.unpack_from("<IIII", img, 0x14000 - 0x120 + 16 * i)
            if magic != 0x544F4C53:
                break
            names.append(hbmod.parse(img[at - 0x02000120:at - 0x02000120 + size])[0]["name"])
        if "CS-1" in names and FWSC == (SRC / "build" / "fm1-dock.fwsc").resolve():   # (the build's own package)
            idx = 1 + names.index("CS-1")
            pick = f"700:8,750:8^,900:e6:{idx},1100:8,1150:8^"
            f4 = wd / "f4.bin"
            out, log = live("shot", 2600, [pick, "1800:9", "1850:9^", "2100:9", "2150:9^"], f4)
            saved = "bank saved" in log
            out, log = live("shot", 1800, [pick], f4)
            check("bank", saved and "bank loaded" in log, "CS-1 saves a user preset (SAVE, SAVE); after a reboot its bank loads")

            # every bank another module's here (CS-1's own further ones): the save is refused, the host says so
            f6 = wd / "f6.bin"
            live("shot", 600, [], f6)
            fl = bytearray(f6.read_bytes())
            key = zlib.crc32(b"CS-1")
            for i in range(9):
                at = 0xEA000 + 0x2000 * i
                fl[at:at + 0x2000] = b"\xff" * 0x2000
                fl[at:at + 48] = struct.pack("<4I", 0x324B4248, key ^ (0x5B000000 | (i + 1)), 32, zlib.crc32(bytes(32))) + bytes(32)
            f6.write_bytes(fl)
            out, log = live("shot", 2600, [pick, "1800:9", "1850:9^", "2100:9", "2150:9^"], f6)
            kept = bytes(f6.read_bytes()[0xEA000:0xFC000]) == bytes(fl[0xEA000:0xFC000])
            check("bank_full", "bank not saved: no bank free" in log and "bank saved" not in log and kept,
                  "with no bank free CS-1's save is refused and logged (the screen: NOT SAVED), the banks as they were")

            # the settings as 0.9 kept them (eight records, 2016 bytes), each with a record: read as they are, CS-1 starts; saved
            # again (OCT+ on the list) the sector is still one 0.9 reads (its length, its CRC), with the same eight and the
            # BLE's among them; a wrong CRC: the sine
            f7 = wd / "f7.bin"
            live("shot", 600, [], f7)
            fl = bytearray(f7.read_bytes())
            keys8 = [zlib.crc32(f"MODULE{r}".encode()) for r in range(7)] + [0x454C4248]
            recs = b"".join(struct.pack("<II", k8, 24) + bytes([r + 1]) * 24 + bytes(216) for r, k8 in enumerate(keys8))
            body = struct.pack("<IiIII", zlib.crc32(b"CS-1"), 0, 0, 0, 0) + recs
            old = struct.pack("<III", 0x53425248, 12 + len(body), zlib.crc32(body)) + body
            fl[SETTINGS:SETTINGS + 0x1000] = old + b"\xff" * (0x1000 - len(old))
            f7.write_bytes(fl)
            out, log = live("shot", 900, [], f7)
            live("shot", 6500, ["900:8,2500:8^", "2800:1", "2850:1^", "3200:8", "3250:8^"], f7)   # (HOME held: the list; OCT+)
            got = f7.read_bytes()[SETTINGS:SETTINGS + 0x1000]
            mg, ln, crc = struct.unpack_from("<III", got)
            kept = {struct.unpack_from("<I", got, 32 + 248 * r)[0]: got[32 + 248 * r + 8:][:24] for r in range(8)}
            want8 = {k8: bytes([r + 1]) * 24 for r, k8 in enumerate(keys8)}
            saved = got[:len(old)] != old
            same = sum(kept.get(k8) == v8 for k8, v8 in want8.items())
            fl[SETTINGS + 8] ^= 1                # (its CRC wrong)
            f7.write_bytes(fl)
            out, log2 = live("shot", 900, [], f7)
            check("settings_old", f"started {idx:08x}" in log and saved and (mg, ln) == (0x53425248, 2016) and crc == zlib.crc32(got[12:2016])
                  and same >= 7 and kept.get(0x454C4248) == want8[0x454C4248] and "started 00000000" in log2 and f"started {idx:08x}" not in log2,
                  f"0.9's settings are read (CS-1 starts); saved again: a sector 0.9 reads ({ln} B), {same} of its 8 records in it as they were, "
                  "the radio's too; with a wrong CRC: the sine")
    finally:
        shutil.rmtree(wd, ignore_errors=True)
    print("all passed" if not failures else f"FAILED: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
