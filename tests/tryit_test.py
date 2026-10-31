#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/tryit.py: the page's Try it files, against firmware/src/dock.c and in fm1-emu's fm1-live.
  tests/tryit_test.py [--deploy]
- the flash layout tryit.py writes is dock.c's (the FM-1's DT_A, DT_B, DP_LO, DP_HI, the magics);
- with fm1-live (FM1_LIVE, default ~/work/fm1-emu/target/release/fm1-live): fm1-dock 0.7 (docs/) boots from a
  fresh flash with a large and a small add-on's .flash laid over it, and their script picks them: the module
  starts (its log line), as the page's Try it does in the browser emulator; a built-in's script picks it;
- --deploy (about a minute): AMB-1 deployed through the DIN jack in fm1-live as tools/dock.py emu --deploy does,
  and its flash afterwards the same as tryit.py's sectors, sector for sector."""
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "tools"))
import tryit  # noqa: E402
import dock  # noqa: E402

FWSC = SRC / "docs" / "fm1-dock-0.7.fwsc"
MODS = SRC / "docs" / "modules"
failures = []


def check(cond, what):
    print(f"{'ok  ' if cond else 'FAIL'} {what}")
    if not cond:
        failures.append(what)


def fm1_layout():
    """the FM-1's constants from dock.c (the #else of #if BOARD_SMK37 with HB_TABLE_AT 0x34000)"""
    c = (SRC / "firmware" / "src" / "dock.c").read_text()
    block = c[c.index("#define HB_TABLE_AT 0x34000u"):]
    val = lambda n, t=block: int(re.search(rf"#define {n}\s+(0x[0-9A-Fa-f]+)u", t).group(1), 16)
    return {k: val(k) for k in ("DT_A", "DT_B", "DP_LO", "DP_HI")} | {
        "DT_MAGIC": val("DT_MAGIC", c), "HB_SLOT_MAGIC": val("HB_SLOT_MAGIC", (SRC / "firmware/dock/hb_module.h").read_text())}


def layout():
    L = fm1_layout()
    check((tryit.DT_A, tryit.DT_B, tryit.DP_LO, tryit.DP_HI) == (L["DT_A"], L["DT_B"], L["DP_LO"], L["DP_HI"]),
          f"the layout is dock.c's: table {L['DT_A']:#x} / {L['DT_B']:#x}, deploy area {L['DP_LO']:#x}..{L['DP_HI']:#x}")
    check(tryit.DT_MAGIC == L["DT_MAGIC"] and tryit.SLOT_MAGIC == L["HB_SLOT_MAGIC"], "the table's and the slot's magics")


def run(flash, keys, ms=4000):
    """fm1-live --shot from flash (a whole chip) with the input script: the host's log"""
    with tempfile.TemporaryDirectory() as wd:
        (Path(wd) / "work").mkdir()
        f = Path(wd) / "flash.bin"
        if flash is not None:
            f.write_bytes(flash)
        r = subprocess.run([dock.live(), "--fwsc", str(FWSC), "--flash", str(f), "--shot", str(ms)], cwd=wd,
                           capture_output=True, text=True,
                           env=dict(os.environ, FM1_BENCH_KEYS=keys, FM1_DUMPRAM=str(Path(wd) / "ram.bin")))
        if r.returncode:
            sys.stderr.write(r.stdout + r.stderr)
            raise SystemExit("fm1-live failed")
        return dock.read_log((Path(wd) / "ram.bin").read_bytes()), (f.read_bytes() if flash is None else None)


def in_emulator():
    _, base = run(None, "", 200)               # the package's fresh flash
    for fid in ("dub", "amb"):
        blob = (MODS / f"{fid}.dock").read_bytes()
        name = tryit.hbmod.parse(blob)[0]["name"]
        flash = bytearray(base)
        for at, data in tryit.sectors_for(blob).items():
            flash[at:at + 4096] = data
        log, _ = run(bytes(flash), tryit.script(name).strip())
        check(re.search(rf"slot ok 0002\w+\n\d+ {re.escape(name)}\n", log) and re.search(rf"started 0000000\d\n\d+ {re.escape(name)}\n", log),
              f"{fid}: {name} in the laid-over flash is checked, and SELECT starts it")
    log, _ = run(base, tryit.script("SCINTILLA").strip())
    check(re.search(r"started 0000000\d\n\d+ SCINTILLA\n", log) is not None, "a built-in's script picks it (SCINTILLA)")


def deployed():
    with tempfile.TemporaryDirectory() as wd:
        fl = Path(wd) / "flash.bin"
        subprocess.run([sys.executable, str(SRC / "tools" / "dock.py"), "emu", "--fwsc", str(FWSC), "--deploy",
                        str(MODS / "amb.dock"), "--ms", "36000", "--flash", str(fl), "--out", str(Path(wd) / "out")],
                       check=True, capture_output=True)
        real = fl.read_bytes()
    secs = tryit.sectors_for((MODS / "amb.dock").read_bytes())
    same = [at for at, d in secs.items() if real[at:at + 4096] == d]
    check(len(same) == len(secs), f"AMB-1 deployed through the DIN jack: {len(same)} of {len(secs)} sectors as tryit.py's")


layout()
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
