#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The banks from the computer (dock.c D_BANKS / D_BANK_READ / D_BANK_WRITE / D_BANK_ERASE; tools/hbdeploy.py) in
fm1-emu: the package booted in fm1-live with a computer on USB (FM1_USB_SEND) and its flash kept (--flash).
  ./build.sh && tests/bank_test.py
  - CS-1 saves a user preset; BANKS lists its bank as CS-1's, BANK_READ gives the bytes the flash has;
  - those bytes written onto another unit (BANK_WRITE): the bank is CS-1's there, and CS-1 loads it after a reboot;
  - written while CS-1 plays: CS-1 is started again and loads it;
  - a wrong CRC, a missing chunk, a synth that is not on the unit, a bank too big: refused, the flash as it was;
  - with every bank taken: no space, the banks as they were;
  - BANK_ERASE gives the bank back; with another key than the bank's it is refused;
  - Scintilla's SAVE, SAVE keeps a sound of the player's own: its bank is listed as SCINTILLA's;
  - a .hbk file packs and unpacks to the same banks.
Needs CS-1 in the package (the Standard one)."""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "tools"))
import dock  # noqa: E402
import hbdeploy as hd  # noqa: E402
import hbmod  # noqa: E402

FWSC = SRC / "build" / "fm1-dock.fwsc"
LO, HI, SIZE = 0xEA000, 0xFC000, 0x2000
failures = []


def check(case, ok, why):
    print(f"  {'ok  ' if ok else 'FAIL'} {case}: {why}")
    if not ok:
        failures.append(case)


def live(ms, keys, flash, usb=()):
    """one fm1-live --shot of the package from flash (left with its flash); usb: [(ms, cmd, payload)], sent with seq =
    their place. -> (the host's log, {seq: [(cmd, payload)]})"""
    with tempfile.TemporaryDirectory() as wd:
        (Path(wd) / "work").mkdir()
        ram = Path(wd) / "ram.bin"
        (Path(wd) / "usb.txt").write_text(";".join(f"{t}:{hd.frame(c, i, p).hex()}" for i, (t, c, p) in enumerate(usb)))
        env = dict(os.environ, FM1_BENCH_KEYS=",".join(keys), FM1_DUMPRAM=str(ram), FM1_USB_SEND="@" + str(Path(wd) / "usb.txt"))
        r = subprocess.run([str(dock.live()), "--fwsc", str(FWSC), "--shot", str(ms), "--flash", str(flash), "--usb"],
                           cwd=wd, capture_output=True, text=True, env=env)
        if r.returncode or "halt" in r.stdout + r.stderr:
            raise SystemExit(f"fm1-live: {r.stdout[-2000:]}{r.stderr[-2000:]}")
        got = {}
        for ln in r.stdout.splitlines():
            if "usb in:" in ln:
                m = hd.parse(bytes.fromhex(ln.split("usb in:")[1].replace(" ", "")))
                if m:
                    got.setdefault(m[1], []).append((m[0], m[2]))
        return dock.read_log(ram.read_bytes()), got


def write(t, name, idx, data, gap=20):
    """BANK_WRITE's frames from t ms on: ([(ms, cmd, payload)], the ms E is in by)"""
    fr = hd.bank_write_frames(name, idx, data)
    return [(t + gap * i, hd.BANK_WRITE, p) for i, p in enumerate(fr)], t + gap * len(fr)


def listed(got, seq):
    out = []
    for c, d in got.get(seq, []):
        if c == hd.BANKS and d[0] == 0 and d[1] != 0xFF:
            key, size, crc, idx = struct.unpack_from("<4I", d, 4)
            out.append(dict(slot=d[1], name=d[20:44].split(b"\0")[0].decode() if d[2] != 0xFF else None, idx=idx, key=key,
                            size=size, crc=crc))
    return out


def st(got, seq):
    return hd.status(got[seq][0][1]) if seq in got else "no answer"


def area(flash):
    return Path(flash).read_bytes()[LO:HI]


def main():
    img = (SRC / "build" / "fm1-dock.bin").read_bytes()
    names = []
    for i in range(32):
        magic, at, size, crc = struct.unpack_from("<IIII", img, 0x14000 - 0x120 + 16 * i)
        if magic != 0x544F4C53:
            break
        names.append(hbmod.parse(img[at - 0x02000120:at - 0x02000120 + size])[0]["name"])
    if "CS-1" not in names:
        raise SystemExit("this package has no CS-1 built in: the Standard one (./build.sh)")
    pick = f"700:8,750:8^,900:e6:{1 + names.index('CS-1')},1100:8,1150:8^"
    save = ["1800:9", "1850:9^", "2100:9", "2150:9^"]
    key = zlib.crc32(b"CS-1")
    wd = Path(tempfile.mkdtemp())
    try:
        # CS-1 saves; the list and the bytes over USB
        a = wd / "a.bin"
        usb = [(2600, hd.BANKS, b"")] + [(2650 + 15 * i, hd.BANK_READ, struct.pack("<B3xI", 0, 240 * i)) for i in range(35)]
        log, got = live(3600, [pick] + save, a, usb)
        bl = listed(got, 0)
        end = [d for c, d in got.get(0, []) if d[1] == 0xFF]
        fl = Path(a).read_bytes()
        size = bl[0]["size"] if bl else 0
        data = b"".join(got[1 + i][0][1][8:] for i in range(35) if 1 + i in got and got[1 + i][0][1][0] == 0)
        check("list", "bank saved" in log and len(bl) == 1 and bl[0]["name"] == "CS-1" and bl[0]["idx"] == 0 and bl[0]["slot"] == 0
              and bl[0]["key"] == key and end and end[0][2] == 9 and end[0][3] == 8,
              f"after CS-1's save BANKS lists one bank, CS-1's, of {size} B; 9 banks, 8 free")
        check("read", size and data == fl[LO + 16:LO + 16 + size] and zlib.crc32(data) == bl[0]["crc"],
              f"BANK_READ gives the bank's {len(data)} bytes as the flash has them, the CRC the list's")
        check("read_past", st(got, 1 + (size + 239) // 240) == "not found", "a read past its end: not found")

        # onto a new unit; CS-1 loads it after a reboot
        mine = bytes(x ^ 0x00 for x in data)
        b = wd / "b.bin"
        fr, t = write(1500, "CS-1", 0, mine)
        log, got = live(t + 1500, [], b, fr + [(t + 600, hd.BANKS, b"")])
        bl = listed(got, len(fr))
        check("write", st(got, len(fr) - 1) == "done" and len(bl) == 1 and bl[0]["name"] == "CS-1" and bl[0]["crc"] == zlib.crc32(mine)
              and area(b)[16:16 + len(mine)] == mine, "the bytes written onto a new unit: done, the bank CS-1's there")
        log, got = live(1800, [pick], b)
        check("load", "bank loaded" in log, "after a reboot CS-1 loads the bank that came from the computer")

        # while CS-1 plays: it is started again
        c = wd / "c.bin"
        fr, t = write(1800, "CS-1", 0, mine)
        log, got = live(t + 1500, [pick], c, fr)
        tail = log.split("bank saved")[-1]
        check("playing", st(got, len(fr) - 1) == "done" and log.count("CS-1") >= 2 and "bank loaded" in tail,
              "written while CS-1 plays: CS-1 is started again and loads it")

        # refused: the flash as it was
        was = area(b)
        fr, t = write(1500, "CS-1", 0, bytes(700))
        bad = list(fr)
        bad[0] = (bad[0][0], hd.BANK_WRITE, bad[0][2][:8] + struct.pack("<I", 1) + bad[0][2][12:])   # another CRC
        log, got = live(t + 1200, [], b, bad)
        check("bad_crc", st(got, len(bad) - 1) == "bad CRC" and area(b) == was, "a bank whose CRC is not its bytes': bad CRC, nothing written")
        log, got = live(t + 1200, [], b, fr[:2] + fr[3:])
        check("missing", st(got, len(fr) - 2) == "data missing" and area(b) == was, "a chunk that never came: data missing, nothing written")
        fr2, t = write(1500, "NOSUCH", 0, bytes(700))
        log, got = live(t + 1200, [], b, fr2)
        check("no_synth", st(got, 0) == "not found" and st(got, len(fr2) - 1) == "not started" and area(b) == was,
              "a bank for a synth that is not on the unit: not found, nothing written")
        big = (1500, hd.BANK_WRITE, b"B\0" + struct.pack("<HII", 0, hd.BANK_MAX + 1, 0) + hd.name24("CS-1"))
        log, got = live(2600, [], b, [big])
        check("too_big", st(got, 0) == "too big", "a bank of more than 8176 bytes: too big")

        # a further bank (idx 2), then every bank another's: no space
        fr, t = write(1500, "CS-1", 2, bytes(range(256)) * 31 + bytes(240))
        log, got = live(t + 1500, [], b, fr + [(t + 600, hd.BANKS, b"")])
        bl = listed(got, len(fr))
        check("idx", st(got, len(fr) - 1) == "done" and sorted((x["name"], x["idx"], x["size"]) for x in bl) ==
              [("CS-1", 0, len(mine)), ("CS-1", 2, 8176)], "a bank of 8176 bytes as CS-1's bank 2: done, listed as that")
        d = wd / "d.bin"
        live(600, [], d)
        fl = bytearray(Path(d).read_bytes())
        for i in range(9):
            at = LO + SIZE * i
            fl[at:at + SIZE] = b"\xff" * SIZE
            fl[at:at + 48] = struct.pack("<4I", 0x324B4248, key ^ (0x5B000000 | (i + 1)), 32, zlib.crc32(bytes(32))) + bytes(32)
        Path(d).write_bytes(fl)
        fr, t = write(1500, "CS-1", 0, mine)
        log, got = live(t + 1500, [], d, fr)
        check("full", st(got, len(fr) - 1) == "no space" and area(d) == bytes(fl[LO:HI]) and "bank not saved: no bank free" in log,
              "with every bank taken (CS-1's own further ones): no space, the banks as they were")

        # given back
        log, got = live(3000, [], b, [(1500, hd.BANK_ERASE, struct.pack("<B3xI", 0, key ^ 1)),
                                      (1600, hd.BANK_ERASE, struct.pack("<B3xI", 0, key)), (2200, hd.BANKS, b"")])
        bl = listed(got, 2)
        check("erase", st(got, 0) == "not found" and st(got, 1) == "done" and [(x["name"], x["idx"]) for x in bl] == [("CS-1", 2)]
              and "bank erased" in log, "BANK_ERASE with another key: not found; with the bank's: gone from the list, the other stays")

        # Scintilla keeps a sound of the player's own (SAVE, SAVE): its bank is listed as SCINTILLA's
        if "SCINTILLA" in names:
            e = wd / "e.bin"
            sc = f"700:8,750:8^,900:e6:{1 + names.index('SCINTILLA')},1100:8,1150:8^"
            log, got = live(4200, [sc, "1700:e2:5", "2100:9", "2150:9^", "2500:9", "2550:9^"], e, [(3400, hd.BANKS, b"")])
            bl = listed(got, 0)
            check("scintilla", "bank saved" in log and [(x["name"], x["idx"]) for x in bl] == [("SCINTILLA", 0)] and bl[0]["size"] == 8 + 12 * 620,
                  f"Scintilla's SAVE, SAVE keeps the sound: one bank, SCINTILLA's, {bl[0]['size'] if bl else '-'} B")

        # the file
        ent = [dict(name="CS-1", idx=0, version=3, data=mine), dict(name="FM-106", idx=2, version=0, data=bytes(8176))]
        check("hbk", hd.hbk_unpack(hd.hbk_pack(ent)) == ent, "a .hbk packs and unpacks to the same banks")
    finally:
        shutil.rmtree(wd, ignore_errors=True)
    print("all passed" if not failures else f"FAILED: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
