#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""fm1-dock's BLE-MIDI (HX, firmware/src/ble/hx.h) in fm1-emu, as far as it goes without a radio model:
  off       the default: HX found and checked, BLUETOOTH OFF, the radio never started; the sine plays
  nocal     BLUETOOTH ON from the list, no stored trims: NO CAL, nothing started; ON kept after a reboot
  trims     stock's VM at 0xE8000 (Optimist's V15 extract): ON runs the radio's start-up program against
            the emulator's empty BT registers within its bounds, then advertises; the sine plays on; after a
            reboot it advertises from boot; 0xE7000-0xE9FFF is left as it was
  safe      OCT+ held at power-on (safe boot) with ON saved: HELD, the radio not started; ON kept
  missing   HX's flash damaged (as a boot firmware leaves it): refused, nothing of HX runs; the sine plays
  none      HX's flash erased (tools/build.py --no-ble): "not in this build", no BLUETOOTH line
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
HX_FILE = 0x15000                               # HX in fm1-dock_xip.bin (XIP 0x02015000, flash 0x19000)
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


def run(case, keys, ms, flash=None, binp=et.APP):
    cmd = [sys.executable, str(SRC / "tools" / "dock.py"), "emu", "--bin", str(binp), "--ms", str(ms),
           "--keys", keys, "--out", str(OUT / case)] + (["--flash", str(flash)] if flash else [])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"{case}: {r.stderr or r.stdout}")
    w = wave.open(str(OUT / case / "audio.wav"))
    b = w.readframes(w.getnframes())
    return struct.unpack(f"<{len(b) // 2}h", b), (OUT / case / "log.txt").read_text()


def states(log, what):
    """HX's states the host logged ("<ms> BLE 00000000"): 'BLE' at boot, 'BLE set' from the list"""
    return [int(v, 16) for v in re.findall(rf"^\s*\d+ {what} ([0-9a-f]{{8}})\s*$", log, re.M)]


def names(st):
    return [ST.get(x, x) for x in st]


def main():
    OUT.mkdir(parents=True, exist_ok=True)

    s, log = run("off", NOTE, 2600)
    st = states(log, "BLE")
    et.check("off", st == [0] and et.peak(s, 1.5, 2.0) > 1000,
             f"boot: HX {names(st)}, the sine's peak {et.peak(s, 1.5, 2.0)}")

    fl = OUT / "nocal.flash"
    fl.unlink(missing_ok=True)
    s, log = run("nocal", toggle(700) + "," + NOTE, 5000, fl)   # (the settings saved 2 s after the last note)
    st = states(log, "BLE set")
    et.check("nocal", st == [1] and et.peak(s, 1.5, 2.0) > 1000,
             f"ON with no trims: {names(st)}, the sine's peak {et.peak(s, 1.5, 2.0)}")
    s, log = run("nocal_again", "", 1200, fl)
    st = states(log, "BLE")
    et.check("nocal_kept", st == [1], f"after a reboot: {names(st)} (ON kept, still no trims)")

    fl = OUT / "trims.flash"
    fl.unlink(missing_ok=True)
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

    bad = OUT / "missing_xip.bin"
    shutil.copy(et.APP, bad)
    b = bytearray(bad.read_bytes())
    b[HX_FILE + 0x400] ^= 0x5A                  # one byte of HX's code
    bad.write_bytes(b)
    s, log = run("missing", NOTE, 2600, binp=bad)
    et.check("missing", "BLE: HX damaged" in log and not states(log, "BLE") and et.peak(s, 1.5, 2.0) > 1000,
             f"HX damaged: {'refused' if 'BLE: HX damaged' in log else 'not noticed'}, "
             f"the sine's peak {et.peak(s, 1.5, 2.0)}")
    hx_end = HX_FILE + ((struct.unpack_from("<I", b, HX_FILE + 4)[0] + 0xFFF) & ~0xFFF)   # (its size, to the next slot)
    b[HX_FILE:hx_end] = b"\xff" * (hx_end - HX_FILE)  # HX's place erased: no HX
    bad.write_bytes(b)
    s, log = run("none", "700:8,750:8^,900:e6:40", 1300, binp=bad)
    et.check("none", "BLE: not in this build" in log and not states(log, "BLE"),
             "HX erased: " + ("not in this build" if "BLE: not in this build" in log else "not noticed"))

    print("all passed" if not et.failures else f"FAILED: {', '.join(et.failures)}")
    return 1 if et.failures else 0


if __name__ == "__main__":
    sys.exit(main())
