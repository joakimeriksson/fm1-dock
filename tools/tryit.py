#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The page's "Try it" (docs/hub.js): every module of the catalog in the browser emulator, fm1-emu's page on the
same site (https://joakimeriksson.github.io/fm1-emu/), running fm1-dock with the module installed and picked.

fm1-emu's page takes, from its own origin only (fm1-emu's web/tryout.js):
  ?flash=URL   flash sectors laid over the package's fresh flash: records of a u32 flash offset (little endian)
               and the 4096-byte sector;
  ?script=URL  an input script on the emulated clock, fm1-live's FM1_BENCH_KEYS: `ms:mHEX` MIDI into the DIN jack.
For each module this writes OUT/<id>.txt, the script: SELECT with the module's name (SAFE for the sine), as
tools/dock.py select sends it, into the DIN jack once fm1-dock has booted. For an add-on also OUT/<id>.flash: the
deployed table and the module as fm1-dock's deploy leaves them on an FM-1 with nothing deployed yet (dock.c's
d_commit: the module relocated to the deploy area's start, as tools/hbmod.py moves one, its table entry in DT_A with
seq 1; DT_B erased), so even a module of 266 KiB is there at boot instead of 100 s of SysEx through the 31250-baud
jack. OUT/index.json lists them for the page.

  tools/tryit.py OUT_DIR [--modules docs/modules] [--fw DIR]    (--fw: the site's fw/, tools/site_packages.py's:
                                                                its index.json says which .dock the site serves)
The layout is the FM-1's in firmware/src/dock.c (checked by tests/tryit_test.py, also against a deploy in fm1-live).
"""
import json
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hbdeploy  # noqa: E402
import hbmod  # noqa: E402

SRC = Path(__file__).resolve().parents[1]
SECTOR = 4096
DT_A, DT_B = 0x93000, 0x94000                   # the deployed table's two copies (dock.c, the FM-1's)
DP_LO, DP_HI = 0x96000, 0xE0000                 # the deploy area
DT_MAGIC = 0x54444248                           # "HBDT"
SLOT_MAGIC = 0x544F4C53                         # "SLOT"
SELECT_AT = 1500                                # ms after the boot: fm1-dock takes SysEx from ~350 ms (the sine up)


def xip(off):
    return 0x02000000 + off - 0x4000            # hal/fm1_xip.h FM1_XIP


def sectors_for(blob):
    """{flash offset: 4096 bytes}: what fm1-dock's deploy of blob into an empty deploy area writes (d_commit)"""
    h, _, _ = hbmod.parse(blob)
    if h["abi"] != hbmod.HB_ABI:
        raise ValueError(f"{h['name']}: built for ABI {h['abi']}, fm1-dock has {hbmod.HB_ABI}")
    at = DP_LO                                  # first fit, nothing deployed
    moved = hbmod.move(blob, xip(at))           # header + relocations + image, relocated to run there
    need = (len(moved) + SECTOR - 1) // SECTOR * SECTOR
    if at + need > DP_HI:
        raise ValueError(f"{h['name']}: {len(moved)} B, larger than the deploy area")
    crc = hbmod.parse(moved)[0]["crc"]
    padded = moved + b"\xff" * (need - len(moved))
    out = {at + o: padded[o:o + SECTOR] for o in range(0, need, SECTOR)}
    slot = struct.pack("<4I", SLOT_MAGIC, xip(at), len(moved), crc)
    seq, n = 1, 1
    head = struct.pack("<4I", DT_MAGIC, seq, n, zlib.crc32(struct.pack("<2I", seq, n) + slot))
    out[DT_A] = (head + slot).ljust(SECTOR, b"\xff")
    out[DT_B] = b"\xff" * SECTOR
    return out


def sector_file(sectors):
    return b"".join(struct.pack("<I", at) + data for at, data in sorted(sectors.items()))


def script(name, builtin=False):
    """SELECT name (SAFE for the host's sine) into the DIN jack, as fm1-live's input script"""
    f = hbdeploy.frame(hbdeploy.SAFE, 1) if builtin else hbdeploy.frame(hbdeploy.SELECT, 1, hbdeploy.name24(name))
    return f"{SELECT_AT}:m{f.hex()}\n"


def dock_of(fid, m, mdir, fw):
    """the .dock the site serves for an add-on: fw/index.json's copy (a release's), else the catalog's own"""
    idx = fw / "index.json" if fw else None
    if idx and idx.exists():
        e = json.loads(idx.read_text()).get("modules", {}).get(fid)
        if e and (fw / e["file"]).exists():
            return fw / e["file"]
    p = mdir / m["file"] if m.get("file") else None
    return p if p and p.exists() else None


def main(argv):
    if not argv or argv[0].startswith("-"):
        raise SystemExit(__doc__)
    out = Path(argv[0])
    mdir = Path(argv[argv.index("--modules") + 1]) if "--modules" in argv else SRC / "docs" / "modules"
    fw = Path(argv[argv.index("--fw") + 1]) if "--fw" in argv else None
    out.mkdir(parents=True, exist_ok=True)
    index = {"firmware": "fm1dock", "modules": {}}
    for fid, m in json.loads((mdir / "modules.json").read_text()).items():
        entry = {"script": f"{fid}.txt"}
        name = m.get("module") or m.get("title")
        if not m.get("packaged"):
            dock = dock_of(fid, m, mdir, fw)
            if not dock:
                print(f"{fid:10} no .dock here: no Try it")
                continue
            blob = dock.read_bytes()
            try:
                secs = sectors_for(blob)
            except ValueError as e:
                print(f"{fid:10} {e}: no Try it")
                continue
            name = hbmod.parse(blob)[0]["name"]
            (out / f"{fid}.flash").write_bytes(sector_file(secs))
            entry["flash"] = f"{fid}.flash"
        (out / f"{fid}.txt").write_text(script(name, bool(m.get("builtin"))))
        entry["module"] = name
        index["modules"][fid] = entry
        print(f"{fid:10} {name:12} {'flash + ' if 'flash' in entry else ''}script")
    (out / "index.json").write_text(json.dumps(index, indent=1) + "\n")


if __name__ == "__main__":
    main(sys.argv[1:])
