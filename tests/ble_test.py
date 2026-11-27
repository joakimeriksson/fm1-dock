#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""fm1-dock's BLE-MIDI (HX, firmware/src/ble/hx.h: the BLUETOOTH add-on, modules/ble) in fm1-emu, as far as it goes
without a radio model. Every case but none runs on a flash with BLUETOOTH added over the DIN jack (as the page adds it):
  added     the add-on deployed: the unit restarts and finds it; BLUETOOTH OFF
  off       the default: HX found and checked, BLUETOOTH OFF, the radio never started; the sine plays
  nocal     BLUETOOTH ON from the list, no stored trims: NO CAL, nothing started; ON kept after a reboot
  trims     stock's VM at 0xE8000 (Optimist's V15 extract): ON runs the radio's start-up program against
            the emulator's empty BT registers within its bounds, then advertises; the sine plays on; after a
            reboot it advertises from boot; 0xE7000-0xE9FFF is left as it was
  safe      OCT+ held at power-on (safe boot) with ON saved: HELD, the radio not started; ON kept
  missing   the add-on's flash damaged: refused, nothing of HX runs; the sine plays
  none      no add-on: "no BLUETOOTH add-on", the line says NOT ADDED
    tests/ble_test.py          (FM1_LIVE: fm1-live, default ~/work/fm1-emu)"""
import re
import shutil
import struct
import subprocess
import sys
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test as et  # noqa: E402

SRC = et.SRC
OUT = SRC / "build" / "emu" / "ble"
BLE_DOCK = SRC / "build" / "modules" / "ble.dock"
HB_F_BLE = 0x2000
DT_A = 0x93000                                  # the deployed table (dock.c DT_A): magic, seq, n, crc, slots
VM_AT = 0xE8000
NOTE = "1600:34,2200:34^"                       # a top-row key: the sine at 1.6 .. 2.2 s
ST = {0: "OFF", 1: "NO CAL", 2: "HELD", 3: "ON", 4: "LINKED", 5: "READY"}

# stock V15's VM after its first boot in an emulator (w0ts/optimist, docs/ble-traces/v15-vm-trim-map.txt,
# area A, its first 0xB0 bytes): the area's magic, records 106, 107, 187, 108, 113, 109
V15_VM = bytes.fromhex("""
55 aa aa 55 91 6a 20 00 0b 0b 2c 6b 70 00 01 07  04 07 0b 01 07 ea bb 40 04 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  00 40 40 00 00 00 00 00 00 00 00 00 00 00 00 00
00 ff ff ff ff ff ff ff ff 00 02 00 02 00 02 00  02 00 02 00 02 00 02 00 02 35 1d 00 00 00 6c 40
01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  00 00 00 00 00 97 71 e0 00 06 1a 00 00 ff ff ff
ff ff ff ff ff ff ff cc 6d 20 02 a1 55 5b 5b 78  78 a6 a6 60 60 60 fe fe fb fb 29 29 37 02 02 ac
ac e4 e4 30 30 30 27 27 56 56 c8 c8 ca ff ff ff""")


def toggle(at):
    """the list (HOME), down to its last line (BLUETOOTH), SEL: its page; SEL on ON / OFF, then HOME twice: at .. at + 1050 ms"""
    return (f"{at}:8,{at + 50}:8^,{at + 200}:e6:40,{at + 400}:3,{at + 450}:3^,{at + 600}:3,{at + 650}:3^,"
            f"{at + 800}:8,{at + 850}:8^,{at + 1000}:8,{at + 1050}:8^")


def run(case, keys, ms, flash=None, binp=et.APP, deploy=None):
    if flash is not None and not Path(flash).exists() and BASE.exists():
        shutil.copy(BASE, flash)                # (a new flash: the one with BLUETOOTH added)
    cmd = [sys.executable, str(SRC / "tools" / "dock.py"), "emu", "--bin", str(binp), "--ms", str(ms),
           "--keys", keys, "--out", str(OUT / case)] + (["--flash", str(flash)] if flash else []) + \
          (["--deploy", str(deploy)] if deploy else [])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"{case}: {r.stderr or r.stdout}")
    w = wave.open(str(OUT / case / "audio.wav"))
    b = w.readframes(w.getnframes())
    return struct.unpack(f"<{len(b) // 2}h", b), (OUT / case / "log.txt").read_text()


BASE = OUT / "base.flash"


def ble_slot(f):
    """the deployed BLUETOOTH add-on in a flash image: (its file's flash offset, its size), else None"""
    magic, seq, n, crc = struct.unpack_from("<4I", f, DT_A)
    for i in range(min(n, 32)):
        m, at, size, c = struct.unpack_from("<4I", f, DT_A + 16 + 16 * i)
        off = at - 0x01FFC000                   # XIP -> flash offset (dock.c FL_OF)
        if m == 0x544F4C53 and 0 <= off < len(f) and struct.unpack_from("<I", f, off + 8)[0] & HB_F_BLE:
            return off, size
    return None


def states(log, what):
    """HX's states the host logged ("<ms> BLE 00000000"): 'BLE' at boot, 'BLE set' from the list"""
    return [int(v, 16) for v in re.findall(rf"^\s*\d+ {what} ([0-9a-f]{{8}})\s*$", log, re.M)]


def names(st):
    return [ST.get(x, x) for x in st]


def main():
    OUT.mkdir(parents=True, exist_ok=True)

    BASE.unlink(missing_ok=True)
    fl = OUT / "added.flash"
    fl.unlink(missing_ok=True)
    s, log = run("added", "", 30000, fl, deploy=BLE_DOCK)   # (deployed over DIN; the unit restarts, then boots with it)
    slot = ble_slot(fl.read_bytes())
    et.check("added", slot is not None and "bluetooth ok" in log, f"BLUETOOTH deployed: {'in the table' if slot else 'not found'}, "
             f"{'found at boot' if 'bluetooth ok' in log else 'not found at boot'}")
    shutil.copy(fl, BASE)

    (OUT / "off.flash").unlink(missing_ok=True)
    s, log = run("off", NOTE, 2600, OUT / "off.flash")
    st = states(log, "BLE")
    et.check("off", st == [0] and et.peak(s, 1.5, 2.0) > 1000,
             f"boot: HX {names(st)}, the sine's peak {et.peak(s, 1.5, 2.0)}")

    fl = OUT / "nocal.flash"
    fl.unlink(missing_ok=True)
    shutil.copy(BASE, fl)
    s, log = run("nocal", toggle(700) + "," + NOTE, 5000, fl)   # (the settings saved 2 s after the last note)
    st = states(log, "BLE set")
    et.check("nocal", st == [1] and et.peak(s, 1.5, 2.0) > 1000,
             f"ON with no trims: {names(st)}, the sine's peak {et.peak(s, 1.5, 2.0)}")
    s, log = run("nocal_again", "", 1200, fl)
    st = states(log, "BLE")
    et.check("nocal_kept", st == [1], f"after a reboot: {names(st)} (ON kept, still no trims)")

    fl = OUT / "trims.flash"
    fl.unlink(missing_ok=True)
    shutil.copy(BASE, fl)
    run("trims_0", "", 600, fl)                 # the chip as fm1-live keeps it, then stock's VM planted in it
    f = bytearray(fl.read_bytes())
    f[VM_AT:VM_AT + len(V15_VM)] = V15_VM
    fl.write_bytes(f)
    vm_before = bytes(f[0xE7000:0xEA000])
    s, log = run("trims", toggle(700) + "," + NOTE, 5000, fl)
    st = states(log, "BLE set")
    et.check("trims", st == [3] and et.peak(s, 1.5, 2.0) > 1000,
             f"ON with stock's trims: {names(st)} (the radio's start-up ran), the sine's peak {et.peak(s, 1.5, 2.0)}")
    et.check("trims_vm_kept", fl.read_bytes()[0xE7000:0xEA000] == vm_before, "0xE7000-0xE9FFF as it was")
    s, log = run("trims_again", NOTE, 2600, fl)
    st = states(log, "BLE")
    et.check("trims_boot", st == [3] and et.peak(s, 1.5, 2.0) > 1000,
             f"after a reboot: {names(st)} from boot, the sine's peak {et.peak(s, 1.5, 2.0)}")

    s, log = run("trims_safe", "0:1," + "1000:1^", 1500, fl)   # (OCT+ from power-on)
    st = states(log, "BLE")
    et.check("safe", st == [2], f"safe boot with ON saved: {names(st)}")

    bad = OUT / "missing.flash"
    f = bytearray(BASE.read_bytes())
    off, size = slot
    f[off + 0x400] ^= 0x5A                      # one byte of the add-on's code
    bad.write_bytes(f)
    s, log = run("missing", NOTE, 2600, bad)
    et.check("missing", "bluetooth refused" in log and not states(log, "BLE") and et.peak(s, 1.5, 2.0) > 1000,
             f"the add-on damaged: {'refused' if 'bluetooth refused' in log else 'not noticed'}, "
             f"the sine's peak {et.peak(s, 1.5, 2.0)}")
    s, log = run("none", "700:8,750:8^,900:e6:40", 1300)   # (no flash kept: nothing added)
    et.check("none", "BLE: no BLUETOOTH add-on" in log and not states(log, "BLE"),
             "no add-on: " + ("said so" if "BLE: no BLUETOOTH add-on" in log else "not noticed"))

    print("all passed" if not et.failures else f"FAILED: {', '.join(et.failures)}")
    return 1 if et.failures else 0


if __name__ == "__main__":
    sys.exit(main())
