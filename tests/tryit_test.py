#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/tryit.py: the page's Try it files, against firmware/src/dock.c and in fm1-emu's fm1-live.
  tests/tryit_test.py [--fwsc PKG] [--deploy]          (PKG: the package booted, default docs/fm1-dock-0.7.fwsc;
                                                        fm1-dock 1.0's, as the page boots it, for the image's room)
- the flash layout tryit.py writes is dock.c's (the FM-1's DT_A, DT_B, DP_LO, DP_HI, the room's end, the magics),
  and its placement dock.c's dp_place: a module and its data pack too big for the plain area together, the pack in the
  image's room from its top, encrypted with the chip key, a table for each deploy in DT_A and DT_B in turn;
- with fm1-live (FM1_LIVE, default ~/work/fm1-emu/target/release/fm1-live): PKG boots from a fresh flash with a large
  and a small add-on's .flash laid over it, and their script picks them: the module starts (its log line), as the
  page's Try it does in the browser emulator; a built-in's script picks it; with a package that leaves a room (1.0):
  its room as dock.c finds it (its log line), and VECTOR37 with its pack V37-WT in the room, started, the pack found,
  a chord heard;
- --deploy (about a minute): AMB-1 deployed through the DIN jack in fm1-live as tools/dock.py emu --deploy does,
  and its flash afterwards the same as tryit.py's sectors, sector for sector; with a room, VECTOR37 and then V37-WT
  deployed so too, every sector tryit.py writes the same, byte for byte, and no other changed but the settings."""
import array
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
import tryit  # noqa: E402
import dock  # noqa: E402

FWSC = Path(sys.argv[sys.argv.index("--fwsc") + 1]) if "--fwsc" in sys.argv else SRC / "docs" / "fm1-dock-0.7.fwsc"
ROOM = tryit.package_room(FWSC)
ROOM = ROOM if ROOM and ROOM[1] < tryit.HB_AREA_HI else None   # (0.7's image ends at the plain window: no room)
SETTINGS_AT = 0x95000                           # dock.c's, the FM-1's: a deploy saves the module picked there
MODS = SRC / "docs" / "modules"
failures = []


def check(cond, what):
    print(f"{'ok  ' if cond else 'FAIL'} {what}")
    if not cond:
        failures.append(what)


def fm1_layout():
    """the FM-1's constants from dock.c (the #else of #if BOARD_SMK37 with HB_AREA_HI 0x93000)"""
    c = (SRC / "firmware" / "src" / "dock.c").read_text()
    h = (SRC / "firmware/dock/hb_module.h").read_text()
    block = c[c.index("#define HB_AREA_HI  0x93000u"):]
    val = lambda n, t=block: int(re.search(rf"#define {n}\s+(0x[0-9A-Fa-f]+|\d+)u", t).group(1), 0)
    return {k: val(k) for k in ("HB_AREA_HI", "DT_A", "DT_B", "DP_LO", "DP_HI", "DP_N")} | {
        "DT_MAGIC": val("DT_MAGIC", c), "HB_TABLE_AT": val("HB_TABLE_AT", c), "HB_SLOT_MAGIC": val("HB_SLOT_MAGIC", h),
        "HB_SLOTS": val("HB_SLOTS", h)}


def layout():
    L = fm1_layout()
    check((tryit.DT_A, tryit.DT_B, tryit.DP_LO, tryit.DP_HI) == (L["DT_A"], L["DT_B"], L["DP_LO"], L["DP_HI"]),
          f"the layout is dock.c's: table {L['DT_A']:#x} / {L['DT_B']:#x}, deploy area {L['DP_LO']:#x}..{L['DP_HI']:#x}")
    check((tryit.HB_AREA_HI, tryit.HB_TABLE_AT, tryit.HB_SLOTS, L["DP_N"]) == (L["HB_AREA_HI"], L["HB_TABLE_AT"], L["HB_SLOTS"], 2),
          f"the image's room up to {L['HB_AREA_HI']:#x}, the built-ins' table at {L['HB_TABLE_AT']:#x}, two areas (the plain one, the room)")
    check(tryit.DT_MAGIC == L["DT_MAGIC"] and tryit.SLOT_MAGIC == L["HB_SLOT_MAGIC"], "the table's and the slot's magics")


def placement():
    """dp_place and d_commit's writes for VECTOR37 and V37-WT, with 1.0's Standard room (0x67000, the key 0x980F)"""
    v, p = (MODS / "vector37.dock").read_bytes(), (MODS / "v37-wt.dock").read_bytes()
    key, lo = 0x980F, 0x67000
    try:
        tryit.sectors_for(v, [p])
        check(False, "VECTOR37 and V37-WT do not fit the plain area together")
    except ValueError:
        check(True, "VECTOR37 and V37-WT do not fit the plain area together")
    secs = tryit.sectors_for(p, [v], (key, lo))   # (either way round: the bigger goes first, as the page sends it)
    tabs = {}
    for at in (tryit.DT_A, tryit.DT_B):
        magic, seq, n, crc = struct.unpack_from("<4I", secs[at])
        body = secs[at][16:16 + 16 * n]
        ok = magic == tryit.DT_MAGIC and zlib.crc32(struct.pack("<2I", seq, n) + body) == crc
        tabs[at] = (seq, [struct.unpack_from("<4I", body, 16 * i) for i in range(n)]) if ok else None
    a, b = tabs[tryit.DT_A], tabs[tryit.DT_B]
    need = lambda blob: (len(tryit.hbmod.move(blob, 0)) + 4095) & ~4095    # noqa: E731
    at_v, at_p = tryit.DP_LO, tryit.HB_AREA_HI - need(p)
    check(a and b and a[0] == 1 and b[0] == 2 and [s[1] for s in a[1]] == [tryit.xip(at_v)]
          and [s[1] for s in b[1]] == [tryit.xip(at_v), tryit.xip(at_p)] and b[1][0] == a[1][0],
          f"two deploys, two tables: DT_A seq 1 VECTOR37 at {at_v:#x}, DT_B seq 2 with V37-WT in the room's top at {at_p:#x}")
    room = bytearray(b"".join(secs[o] for o in range(at_p, tryit.HB_AREA_HI, 4096)))
    size = b[1][1][2]
    plain = tryit.fm1_install._sfc(bytes(room[:size]), at_p - 0x4000, key)
    h, relocs, image = tryit.hbmod.parse(plain)
    check(h["name"] == "V37-WT" and tryit.hbmod.crc_of(plain[:76], relocs, image) == h["crc"] == b[1][1][3]
          and room[size:] == b"\xff" * (len(room) - size) and room[:16] != plain[:16],
          "V37-WT in the room: encrypted with the chip key, read back through the SFC it checks out (its CRC, the table's)")
    try:
        tryit.sectors_for(v, [p], (key, 0x80000))
        check(False, "a room too small for V37-WT: no Try it")
    except ValueError:
        check(True, "a room too small for V37-WT: no Try it")
    if ROOM:
        check(ROOM[1] % 4096 == 0 and tryit.HB_TABLE_AT < ROOM[1] < tryit.HB_AREA_HI,
              f"{FWSC.name}: the image's room {ROOM[1]:#x}..{tryit.HB_AREA_HI:#x}, the chip key {ROOM[0]:#06x}")


def run(flash, keys, ms=4000, audio=False):
    """fm1-live --shot (audio: --bench) from flash (a whole chip) with the input script: the host's log, the flash
    afterwards, the sound (44.1 kHz stereo s16le)"""
    with tempfile.TemporaryDirectory() as wd:
        (Path(wd) / "work").mkdir()
        f, wav = Path(wd) / "flash.bin", Path(wd) / "audio.raw"
        if flash is not None:
            f.write_bytes(flash)
        r = subprocess.run([dock.live(), "--fwsc", str(FWSC), "--flash", str(f), "--bench" if audio else "--shot", str(ms)],
                           cwd=wd, capture_output=True, text=True,
                           env=dict(os.environ, FM1_BENCH_KEYS=keys, FM1_DUMPRAM=str(Path(wd) / "ram.bin"),
                                    FM1_BENCH_WAV=str(wav)))
        if r.returncode:
            sys.stderr.write(r.stdout + r.stderr)
            raise SystemExit("fm1-live failed")
        return (dock.read_log((Path(wd) / "ram.bin").read_bytes()), f.read_bytes(),
                array.array("h", wav.read_bytes()) if audio else None)


def laid(base, secs):
    flash = bytearray(base)
    for at, data in secs.items():
        flash[at:at + 4096] = data
    return bytes(flash)


def peak(pcm, t0, t1):
    return max((abs(x) for x in pcm[int(t0 * 88.2) & ~1:int(t1 * 88.2) & ~1]), default=0)


def in_emulator():
    _, base, _ = run(None, "", 200)            # the package's fresh flash
    for fid in ("dub", "amb"):
        blob = (MODS / f"{fid}.dock").read_bytes()
        name = tryit.hbmod.parse(blob)[0]["name"]
        log, _, _ = run(laid(base, tryit.sectors_for(blob, (), ROOM)), tryit.script(name).strip())
        check(re.search(rf"slot ok 0002\w+\n\d+ {re.escape(name)}\n", log) and re.search(rf"started 0000000\d\n\d+ {re.escape(name)}\n", log),
              f"{fid}: {name} in the laid-over flash is checked, and SELECT starts it")
    log, _, _ = run(base, tryit.script("SCINTILLA").strip())
    check(re.search(r"started 0000000\d\n\d+ SCINTILLA\n", log) is not None, "a built-in's script picks it (SCINTILLA)")
    if not ROOM:
        print(f"({FWSC.name} leaves no room: VECTOR37 is not tried)")
        return
    check(f"room: the image ends at {ROOM[1]:08x}" in log, f"fm1-dock finds the room where tryit.py does: {ROOM[1]:#x}")
    v, p = (MODS / "vector37.dock").read_bytes(), (MODS / "v37-wt.dock").read_bytes()
    chord = ",".join(f"3000:{n}@100,4500:{n}^" for n in ("C4", "E4", "G4"))
    log, _, pcm = run(laid(base, tryit.sectors_for(v, [p], ROOM)), tryit.script("VECTOR37").strip() + "," + chord, 5000, True)
    started = re.search(r"\d+ vector37: (.*)\n\d+ started 0000000\d\n\d+ VECTOR37\n", log)
    for line in re.findall(r"\d+ (?:slot ok|pack ok|room:|vector37:).*\n(?:\d+ [A-Z0-9 -]+\n)?", log):
        print("     " + line.strip().replace("\n", " / "))
    check(re.search(r"slot ok 0002\w+\n\d+ VECTOR37\n", log) and re.search(r"pack ok 0002\w+\n\d+ V37-WT\n", log)
          and started and "no wavetables" not in started.group(1),
          "VECTOR37 and V37-WT (in the room) from tryit.py's flash: both check out, SELECT starts VECTOR37 with its wavetables")
    quiet, loud = peak(pcm, 2000, 2900), peak(pcm, 3100, 4400)
    check(loud > 1000 and quiet < loud / 10, f"a chord on VECTOR37 is heard: peak {loud} (before it {quiet})")


def deployed():
    with tempfile.TemporaryDirectory() as wd:
        fl = Path(wd) / "flash.bin"
        subprocess.run([sys.executable, str(SRC / "tools" / "dock.py"), "emu", "--fwsc", str(FWSC), "--deploy",
                        str(MODS / "amb.dock"), "--ms", "36000", "--flash", str(fl), "--out", str(Path(wd) / "out")],
                       check=True, capture_output=True)
        real = fl.read_bytes()
    secs = tryit.sectors_for((MODS / "amb.dock").read_bytes(), (), ROOM)
    same = [at for at, d in secs.items() if real[at:at + 4096] == d]
    check(len(same) == len(secs), f"AMB-1 deployed through the DIN jack: {len(same)} of {len(secs)} sectors as tryit.py's")
    if not ROOM:
        return
    v, p = (MODS / "vector37.dock").read_bytes(), (MODS / "v37-wt.dock").read_bytes()
    e1, t1 = tryit.hbdeploy.din_script(v, 1000)            # VECTOR37, then its pack, as the page sends them
    e2, t2 = tryit.hbdeploy.din_script(p, t1 + 6000)
    _, base, _ = run(None, "", 200)
    log, real, _ = run(base, ",".join(e1 + e2), int(t2) + 6000)
    check(log.count("deploy: done") == 2, "VECTOR37 and V37-WT deployed through the DIN jack (both done)")
    secs = tryit.sectors_for(v, [p], ROOM)
    same = [at for at, d in secs.items() if real[at:at + 4096] == d]
    other = [o for o in range(0, len(real), 4096) if o not in secs and o != SETTINGS_AT and real[o:o + 4096] != base[o:o + 4096]]
    check(len(same) == len(secs) and not other,
          f"VECTOR37 + V37-WT deployed: {len(same)} of {len(secs)} sectors byte for byte as tryit.py's "
          f"({sum(1 for a in secs if a < tryit.HB_AREA_HI)} in the room), no other sector changed but the settings"
          + (f" (also {', '.join(hex(o) for o in other)})" if other else ""))


layout()
placement()
try:
    dock.live()
except SystemExit as e:
    print(f"({e}: the emulator checks are skipped)")
else:
    in_emulator()
    if "--deploy" in sys.argv:
        deployed()
print(f"TRYIT TESTS FAILED ({len(failures)})" if failures else "tryit tests passed")
sys.exit(1 if failures else 0)
