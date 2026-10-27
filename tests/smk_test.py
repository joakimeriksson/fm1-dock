#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""fm1-dock on the M-Vave SMK-37 in fm1-emu (fm1-live, headless; its SMK-37 board from the package's identity):
  ./build.sh --board smk37 && tests/smk_test.py        (FM1_LIVE: an fm1-live that takes SMK-37 velocities, the
                                                       analog inputs `ms:aN:V` and FM1_USB_SEND=@file)
  - boot: the unit's key curves read, the three modules of the package checked, BLUETOOTH OFF; a key plays the sine;
  - the radio's trims: stock's VM (as stock v016 writes it) anywhere in its region is found and copied into the
    settings at boot, at its start (0x9D000) and in its last sector (0xBF000);
  - keys: the middle 27 play as the FM-1's, the outer ones (C3, C6) as notes beyond them, with their velocity;
    CS-1 (picked with PATCH, K2 and SCALE) plays a hard key louder than a soft one;
  - extras over USB-MIDI: the pads (notes 36..51, channel 10), a fader (CC 70), the mod wheel (CC 1), the pitch
    wheel (the bend, and back to the centre);
  - deploys over USB (the SMK-37 has no MIDI in jack): FM-1 SID into the first area (from 0x70000), AMB-1 into the
    second (from 0xC2000, above the calibration); the stock firmware's mapping marker goes first; both run; the
    protected ranges (the head, BTIF and the calibration, the stock resources, key_mac) are as they were;
  - BLE-MIDI: FM-1 SID wrote over stock's VM; BLUETOOTH ON from the list starts the radio from the copy (as far as
    fm1-emu goes: its start-up program against the emulator's BT registers, then advertising), and from boot after;
  - OCTAVE- held at power-on: the USB rescue, which answers the installers as an SMK-37 (SMK-37 Pro_000)."""
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "tools"))
import dock  # noqa: E402
import hbdeploy  # noqa: E402
import hbmod  # noqa: E402

FWSC = SRC / "build" / "fm1-dock-smk37.fwsc"
DT = (0xB8000, 0xB9000)
AREA1, AREA2 = (0x70000, 0xB8000), (0xC2000, 0xE2000)
PROTECTED = [(0x0, 0x4000), (0xC0000, 0xC2000), (0xF4000, 0xFE000), (0xFF000, 0x100000)]
STOCKTAG = 0xEA000
HS_QUERY = "f0003245000000407ff7"
# stock SMK-37 Pro v016's VM after its first boot in fm1-emu, at the start of its region (0x9D000), its first 0xB0
# bytes: the area's magic, records 106, 107, 187, 108, 113, 109 (106 / 107 / 108 / 113 as the FM-1's, measured on one)
SMK_VM = bytes.fromhex("""
55 aa aa 55 91 6a 20 00 0b 0b 2c 6b 70 00 01 07  04 07 0b 01 07 9f bb 40 04 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  00 40 40 00 00 00 00 00 00 00 00 00 00 00 00 00
00 ff 80 80 80 ff 80 80 80 00 02 00 02 00 02 00  02 00 02 00 02 00 02 00 02 9f c5 00 00 00 6c 40
01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  00 00 00 00 00 97 71 e0 00 06 1a 00 00 ff ff ff
ff ff ff ff ff ff ff 71 6d 20 02 a1 55 36 74 ec  79 cf ea 8b 8e 15 03 fd 9e 1f ff b8 75 61 cc 4a
fb f2 70 e2 8d ab 1f 11 80 10 43 e6 bd ff ff ff""")
ST = {0: "OFF", 1: "NO CAL", 2: "HELD", 3: "ON", 4: "LINKED", 5: "READY"}
failures = []


def check(case, ok, why):
    print(f"  {'ok  ' if ok else 'FAIL'} {case}: {why}")
    if not ok:
        failures.append(case)


def live(ms, keys, flash, usb=None):
    """one fm1-live --shot of the package from flash (left with its flash): (stdout, the host's log, [USB-MIDI out])"""
    with tempfile.TemporaryDirectory() as wd:
        (Path(wd) / "work").mkdir()
        env = dict(os.environ, FM1_BENCH_KEYS=",".join(keys), FM1_DUMPRAM=str(Path(wd) / "ram.bin"))
        args = [str(dock.live()), "--fwsc", str(FWSC), "--shot", str(ms), "--flash", str(flash)]
        if usb is not None:
            (Path(wd) / "usb.txt").write_text(";".join(usb))
            env["FM1_USB_SEND"] = "@" + str(Path(wd) / "usb.txt")
            args.append("--usb")
        r = subprocess.run(args, cwd=wd, capture_output=True, text=True, env=env)
        if r.returncode or "halt" in r.stdout + r.stderr:
            raise SystemExit(f"fm1-live: {r.stdout[-2000:]}{r.stderr[-2000:]}")
        ram = Path(wd) / "ram.bin"
        log = dock.read_log(ram.read_bytes()) if ram.exists() else ""
        out = [bytes.fromhex(ln.split("usb in:")[1].replace(" ", "")) for ln in r.stdout.splitlines() if "usb in:" in ln]
        return r.stdout, log, out


def peak(out):
    return int(out.split("audio peak over the last 0.5 s: ")[1].split()[0]) if "audio peak" in out else 0


def usb_deploy(blob, t0=1500, gap=15):
    """the deploy's frames over USB at times (ms): each DATA twice, as the computer sends again a chunk that was not
    answered (the device takes one it has again); -> (FM1_USB_SEND entries, ms the COMMIT is in by)"""
    out, t = [], t0
    for i, (cmd, payload) in enumerate(hbdeploy.deploy_frames(blob)):
        if cmd == hbdeploy.COMMIT:
            t += 400
        for _ in range(2 if cmd == hbdeploy.DATA else 1):
            out.append(f"{t}:{hbdeploy.frame(cmd, i, payload).hex()}")
            t += 600 if cmd == hbdeploy.BEGIN else gap
    return out, t + 3000


def committed(usb_out):
    for m in usb_out:
        r = hbdeploy.parse(m)
        if r and r[0] == hbdeploy.COMMIT:
            return hbdeploy.status(r[2])
    return "no answer"


def table(flash):
    """the deployed modules in force: [(name, flash offset, CRC ok)]"""
    best = None
    for at in DT:
        magic, seq, n, crc = struct.unpack_from("<4I", flash, at)
        if magic != 0x54444248 or n > 32 or zlib.crc32(struct.pack("<II", seq, n) + flash[at + 16:at + 16 + 16 * n]) != crc:
            continue
        if best is None or (seq - best[0]) & 0xFFFFFFFF < 0x80000000:
            best = (seq, flash[at + 16:at + 16 + 16 * n])
    out = []
    for i in range(len(best[1]) // 16 if best else 0):
        _, xip, size, crc = struct.unpack_from("<4I", best[1], 16 * i)
        off = xip - 0x02000000 + 0x4000
        h, relocs, image = hbmod.parse(flash[off:off + size])
        out.append((h["name"], off, hbmod.crc_of(flash[off:off + 76], relocs, image) == h["crc"] == crc))
    return out


def states(log, what):
    """HX's states the host logged: 'BLE' at boot, 'BLE set' from the list"""
    return [ST.get(int(v, 16), v) for v in re.findall(rf"^\s*\d+ {what} ([0-9a-f]{{8}})\s*$", log, re.M)]


def toggle(at):
    """BLUETOOTH from the list: PATCH held (HOME, past a module's own panel), K2 (SELECT) down to its last line,
    SCALE (SEL), PATCH: at .. at + 2950 ms"""
    return [f"{at}:patch", f"{at + 1800}:patch^", f"{at + 2100}:e1:40", f"{at + 2600}:scale", f"{at + 2650}:scale^",
            f"{at + 2900}:patch", f"{at + 2950}:patch^"]


def planted(src, at):
    """a copy of the flash file src with stock's VM at at"""
    f = bytearray(src.read_bytes())
    f[at:at + len(SMK_VM)] = SMK_VM
    dst = src.with_name(f"vm{at:x}.bin")
    dst.write_bytes(bytes(f))
    return dst


def notes(usb_out):
    return [m.hex() for m in usb_out if len(m) == 3 and m[0] & 0xE0 == 0x80]


def main():
    if not FWSC.exists():
        raise SystemExit(f"{FWSC} is missing: ./build.sh --board smk37")
    with tempfile.TemporaryDirectory() as td:
        flash = Path(td) / "flash.bin"
        print("boot")
        out, log, _ = live(1500, ["900:k12"], flash)
        check("curves", "SMK-37: the factory key curves" in log, "the unit's key curves read (USRTRIM, 0xC1000)")
        check("modules", sum("slot ok" in ln for ln in log.splitlines()) == 3, "CS-1, Scintilla and OberVave checked")
        check("ble", states(log, "BLE") == ["OFF"] and "BLE: no radio trims" in log,
              f"HX checked, BLUETOOTH {states(log, 'BLE')}; no stock VM in a new flash")
        check("sine", peak(out) > 1000, f"a middle key plays the sine (peak {peak(out)})")
        base = flash.read_bytes()

        print("the radio's trims")
        for at in (0xBF000, 0x9D000):           # (0x9D000 last: its flash goes on to the deploys)
            fl = planted(flash, at)
            _, log, _ = live(1200, [], fl)
            again = live(1200, [], fl)[1]
            check(f"trims {at:#x}", f"BLE: the radio's trims kept, from {at:08x}" in log and "trims kept" not in again,
                  f"stock's VM at {at:#x} found, copied once ({'kept' if 'trims kept' in log else 'not found'})")
        flash.write_bytes(fl.read_bytes())

        print("keys and the extras")
        keys = ["900:k0@100", "1100:k0^", "1300:k36@60", "1500:k36^", "1700:a0:700", "2000:a0:0", "2200:a9:400",
                "2500:a9:0", "2700:a16:800", "3200:a21:300", "3600:a21:629", "3900:a20:950", "4400:a20:535"]
        _, _, usb = live(5000, keys, flash, usb=[])
        ns = notes(usb)
        check("outer", any(n.startswith("9030") for n in ns) and any(n.startswith("9054") for n in ns),
              f"C3 and C6 play notes 48 and 84 ({', '.join(n for n in ns if n[:2] in ('90', '80'))})")
        check("pads", "992c" in "".join(n[:4] for n in ns) and "9925" in "".join(n[:4] for n in ns) and
              any(n.startswith("892c") for n in ns), "the top-left pad note 44, the bottom row's second 37, channel 10")
        ccs = [m.hex() for m in usb if m[0] == 0xB0]
        check("fader", any(c.startswith("b046") for c in ccs), f"fader 1: CC 70 (last {[c for c in ccs if c.startswith('b046')][-1:]})")
        check("mod", "b0017f" in ccs and "b00100" in ccs, "the mod wheel: CC 1 to 127 and back to 0")
        bends = [m.hex() for m in usb if m[0] == 0xE0]
        check("bend", "e07f7f" in bends and bends[-1:] == ["e00040"], f"the pitch wheel: full up, then the centre ({len(bends)} bends)")

        print("CS-1's velocity")
        pick = ["900:patch", "1000:patch^", "1400:e1:2", "1900:scale", "2000:scale^"]
        soft = peak(live(2900, pick + ["2400:k12@15"], flash)[0])
        hard = peak(live(2900, pick + ["2400:k12@127"], flash)[0])
        check("velocity", hard > soft * 1.3 > 0, f"a hard key louder than a soft one ({hard} > {soft})")

        print("deploys over USB")
        f = bytearray(flash.read_bytes())
        f[STOCKTAG + 0xFF8:STOCKTAG + 0x1000] = b"usrdata\0"      # the stock firmware's mapping marker
        flash.write_bytes(bytes(f))
        before = flash.read_bytes()
        for name, area in (("fm1sid.dock", AREA1), ("amb.dock", AREA2)):
            send, ms = usb_deploy((SRC / "docs" / "modules" / name).read_bytes())
            _, _, usb = live(ms, [], flash, usb=send)
            st = committed(usb)
            t = table(flash.read_bytes())
            mine = [e for e in t if e[0] == hbmod.parse((SRC / "docs" / "modules" / name).read_bytes())[0]["name"]]
            check(name, st == "done" and mine and mine[0][2] and area[0] <= mine[0][1] < area[1],
                  f"COMMIT {st}, at {mine[0][1]:#x} ({'area 1' if area == AREA1 else 'area 2'})" if mine else f"COMMIT {st}")
        after = flash.read_bytes()
        check("marker", after[STOCKTAG:STOCKTAG + 0x1000] == b"\xff" * 0x1000,
              "the stock mapping's marker sector erased before USRFLASH was written")
        check("protected", all(before[lo:hi] == after[lo:hi] == base[lo:hi] for lo, hi in PROTECTED),
              "the head, BTIF + calibration, the stock resources and key_mac unchanged")
        out, log, _ = live(9000, ["900:patch", "2700:patch^", "3000:e1:-40", "3900:e1:5", "4500:scale", "4600:scale^"],
                           flash)                # (the list: the sine, CS-1, Scintilla, OberVave, FM-1 SID, AMB-1, BLUETOOTH)
        check("both", sum(n in log for n in ("FM-1 SID", "AMB-1")) == 2 and "started 00000005" in log and peak(out) > 100
              and not states(log, "BLE set"),
              f"FM-1 SID and AMB-1 in the list; AMB-1 from the second area plays (peak {peak(out)})")

        print("BLE-MIDI")
        gone = after[0x9D000:0x9D000 + len(SMK_VM)] != SMK_VM
        out, log, _ = live(6500, toggle(900) + ["4800:k12", "5500:k12^"], flash)
        check("ble on", gone and states(log, "BLE set") == ["ON"] and peak(out) > 100,
              f"stock's VM written over ({gone}); BLUETOOTH ON from the list: {states(log, 'BLE set')} (the radio's "
              f"start-up ran, from the copy); the module plays on (peak {peak(out)})")
        out, log, _ = live(5000, [], flash)
        check("ble boot", states(log, "BLE") == ["ON"], f"after a reboot: {states(log, 'BLE')} from boot")

        print("rescue")
        _, _, usb = live(2500, ["0:octave-", "2400:octave-^"], flash, usb=[f"1800:{HS_QUERY}"])
        ids = [hbdeploy.unpack7(m[1:-1])[6:33].rstrip(b"\0").decode("latin-1") for m in usb
               if m[:1] == b"\xf0" and len(hbdeploy.unpack7(m[1:-1])) == 34]
        check("rescue", "SMK-37 Pro_000" in ids, f"OCTAVE- at power-on: the USB rescue, answering as {ids}")
    print("smk tests " + ("FAILED: " + ", ".join(failures) if failures else "passed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
