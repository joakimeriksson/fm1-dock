#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The page's "Try it" (docs/hub.js): every module of the catalog in the browser emulator, fm1-emu's page on the
same site (https://joakimeriksson.github.io/fm1-emu/), running fm1-dock with the module installed and picked.

fm1-emu's page takes, from its own origin only (fm1-emu's web/tryout.js):
  ?flash=URL   flash sectors laid over the package's fresh flash: records of a u32 flash offset (little endian)
               and the 4096-byte sector;
  ?script=URL  an input script on the emulated clock, fm1-live's FM1_BENCH_KEYS: `ms:mHEX` MIDI into the DIN jack;
  ?package=URL the package it boots (https), here the site's copy of fm1-dock's latest release (fw/), whose deploy
               areas the flash is laid out for (without it the page boots its own list's fm1-dock).
For each module this writes OUT/<id>.txt, the script: SELECT with the module's name (SAFE for the sine), as
tools/dock.py select sends it, into the DIN jack once fm1-dock has booted. For an add-on also OUT/<id>.flash: the
deployed tables and the module with its data packs (its "packs" in modules.json) as fm1-dock's deploys of them leave
them on an FM-1 with nothing deployed yet, sent as the page sends them (the bigger first), so even 300 KiB of them are
there at boot instead of minutes of SysEx through the 31250-baud jack. Each deploy is dock.c's d_commit: dp_place's
first fit (the plain area first, from its start; then the image's room, from its top down), the module relocated
to run there (as tools/hbmod.py moves one), in the room encrypted with the chip key as an update writes it, and a
new table each time, into DT_A and DT_B in turn (seq 1, 2, ...). The room is the package's (dock.c ir_init: from the
image's end as its JLFS says, with the chip key of its isd_config.ini, up to 0x93000). OUT/index.json lists them for
the page, and the package they are laid out for.

  tools/tryit.py OUT_DIR [--modules docs/modules] [--fw DIR] [--fwsc PKG]
      --fw: the site's fw/ (tools/site_packages.py's): its index.json says which .dock files the site serves, and
            its fm1-dock package is the one the page boots and the flash is laid out for (its room);
      --fwsc: that package from elsewhere (no package: the plain area only, the page's own fm1-dock booted)
The layout is the FM-1's in firmware/src/dock.c (checked by tests/tryit_test.py, also against deploys in fm1-live).
"""
import json
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fm1_install  # noqa: E402
import hbdeploy  # noqa: E402
import hbmod  # noqa: E402

SRC = Path(__file__).resolve().parents[1]
SECTOR = 4096
DT_A, DT_B = 0x93000, 0x94000                   # the deployed table's two copies (dock.c, the FM-1's)
DP_LO, DP_HI = 0x96000, 0xE0000                 # the deploy area in the plain window
HB_AREA_HI = 0x93000                            # the image's room's end (the end of what an update may write)
HB_TABLE_AT = 0x18000                           # the image's slot table (its built-ins)
DT_MAGIC = 0x54444248                           # "HBDT"
SLOT_MAGIC = 0x544F4C53                         # "SLOT"
HB_SLOTS = 32
SELECT_AT = 1500                                # ms after the boot: fm1-dock takes SysEx from ~350 ms (the sine up)


def xip(off):
    return 0x02000000 + off - 0x4000            # hal/fm1_xip.h FM1_XIP


def _u16(b, o):
    return int.from_bytes(b[o:o + 2], "little")


def _u32(b, o):
    return int.from_bytes(b[o:o + 4], "little")


def chip_key(flash):
    """the chip key, as dock.c's ota_chip_key reads it from flash: the boot area's isd_config.ini blob; None: none"""
    for off in range(32, 0x400, 32):
        e = fm1_install._dec(flash[off:off + 32])
        if fm1_install._crc16(e[2:32]) != _u16(e, 0):
            return None
        if e[16:31] != b"isd_config.ini\0":
            if _u16(e, 14):
                return None
            continue
        blob = flash[_u32(e, 4):_u32(e, 4) + 34]
        if len(blob) < 34 or fm1_install._crc16(blob[:32]) != _u16(blob, 32):
            return None
        s = sum(blob[:16]) & 0xFF
        s = 0xAA if s >= 0xE0 else 0x55 if s <= 0x10 else s
        return sum(1 << i for i in range(16) if (blob[16 + i] ^ blob[15 - i]) < s)
    return None


def flash_room(flash):
    """(chip key, the room's start) of a flash image as installed, as dock.c's ir_init finds them (ota_area_end: the
    app area's JLFS, app_area_head then its cfg dir, 4 KiB up; the built-ins inside it); None: no room"""
    key = chip_key(flash)
    if key is None:
        return None
    h = fm1_install._sfc(flash[0x4000:0x4020], 0, key)
    if fm1_install._crc16(h[2:32]) != _u16(h, 0) or h[16:30] != b"app_area_head\0":
        return None
    blk = _u32(h, 8)
    if blk < 0x140 or blk > 0x100000 - 0x4000:
        return None
    c = fm1_install._sfc(flash[0x4000 + (blk & ~31):0x4000 + (blk & ~31) + 64], blk & ~31, key)[blk & 31:]
    cfg = _u32(c, 8)
    if fm1_install._crc16(c[2:32]) != _u16(c, 0) or c[16:20] != b"cfg\0" or cfg > 65536:
        return None
    end = (0x4000 + blk + cfg + 0xFFF) & ~0xFFF
    t = fm1_install._sfc(flash[HB_TABLE_AT:HB_TABLE_AT + 16 * HB_SLOTS], HB_TABLE_AT - 0x4000, key)
    for magic, at, size, _ in struct.iter_unpack("<4I", t):
        if magic != SLOT_MAGIC:
            break
        if at - xip(0) + size > end:
            return None
    if end < HB_TABLE_AT + 0x1000 or end > HB_AREA_HI:
        return None
    return key, end


def package_room(path):
    """flash_room of a package's flash.bin (laid at flash 0 by an update)"""
    _, image = fm1_install.load_package(str(path), False)
    fl = next((f for f in fm1_install.package_files(image) if f[0] == 0), None)
    return flash_room(image[fl[1]:fl[1] + fl[2]]) if fl else None


def _up(n):
    return (n + SECTOR - 1) // SECTOR * SECTOR


def dp_place(need, slots, areas):
    """dock.c's dp_place: where a module of need bytes goes (0: nowhere), slots the table's [(flash offset, size)],
    areas [plain, the image's room]: the plain area first, first fit from its start; the room last, from its top down"""
    n_a, ir = len(areas), len(areas) - 1
    area = lambda off: next((a for a, (lo, hi) in enumerate(areas) if lo <= off < hi), n_a)  # noqa: E731

    def room(at):                               # dock.c dp_room
        end = areas[area(at) % n_a][1]
        for lo, size in slots:
            if lo <= at < lo + _up(size):
                return 0
            if at <= lo < end:
                end = lo
        return end - at
    at, at_a = 0, n_a
    for a in range(n_a):
        for i in range(len(slots) + 1):
            s0 = slots[i][0] + _up(slots[i][1]) if i < len(slots) else areas[a][0]
            if areas[a][0] >= areas[a][1] or area(s0) != a:
                continue
            r = room(s0)
            if not r or r < need or a > at_a:
                continue
            if a == ir:
                if at_a != ir or s0 + r - need > at:
                    at, at_a = s0 + r - need, a
            elif a < at_a or s0 < at:
                at, at_a = s0, a
    return at


def sectors_for(blob, packs=(), room=None):
    """{flash offset: 4096 bytes}: what fm1-dock's deploys of blob and its data packs (README "Data packs"), the bigger
    first as the page sends them, write on an FM-1 with nothing deployed (d_commit each); room: (chip key, the room's
    start) of the package (package_room), None: the plain area only"""
    areas = [(DP_LO, DP_HI), (room[1], HB_AREA_HI) if room else (0, 0)]
    slots, table, out = [], [], {DT_B: b"\xff" * SECTOR}
    for b in sorted((blob, *packs), key=len, reverse=True):
        h, _, _ = hbmod.parse(b)
        if h["abi"] != hbmod.HB_ABI:
            raise ValueError(f"{h['name']}: built for ABI {h['abi']}, fm1-dock has {hbmod.HB_ABI}")
        total = len(hbmod.move(b, xip(DP_LO)))
        at = dp_place(_up(total), slots, areas)
        if not at:
            raise ValueError(f"{h['name']}: {total} B, no room left for it" + ("" if room else " in the plain area"))
        moved = hbmod.move(b, xip(at))          # header + relocations + image, relocated to run there
        crc = hbmod.parse(moved)[0]["crc"]
        if areas[1][0] <= at < areas[1][1]:     # the image's room: encrypted as an update writes (ota_sfc)
            moved = fm1_install._sfc(moved, at - 0x4000, room[0])
        padded = moved + b"\xff" * (_up(total) - total)
        out.update({at + o: padded[o:o + SECTOR] for o in range(0, len(padded), SECTOR)})
        slots.append((at, total))
        table.append(struct.pack("<4I", SLOT_MAGIC, xip(at), total, crc))
        seq, n = len(table), len(table)          # dt_write: the next sequence number, into the other copy
        head = struct.pack("<4I", DT_MAGIC, seq, n, zlib.crc32(struct.pack("<2I", seq, n) + b"".join(table)))
        out[DT_A if seq % 2 else DT_B] = (head + b"".join(table)).ljust(SECTOR, b"\xff")
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
    pkg = Path(argv[argv.index("--fwsc") + 1]) if "--fwsc" in argv else None
    out.mkdir(parents=True, exist_ok=True)
    index = {"firmware": "fm1dock", "modules": {}}
    if not pkg and fw and (fw / "index.json").exists():   # the site's fm1-dock: the page boots it (?package=)
        e = json.loads((fw / "index.json").read_text()).get("firmwares", {}).get("fm1-dock")
        if e and (fw / e["file"]).exists():
            pkg = fw / e["file"]
            index["package"] = {"file": f"../{fw.name}/{e['file']}", "name": f"fm1-dock {e.get('version', '')}".strip()}
    room = package_room(pkg) if pkg else None
    if room and "package" in index:             # (what the flash is laid out for: the room's start, the chip key)
        index["package"] |= {"room": room[1], "key": room[0]}
    print(f"package {pkg.name if pkg else '(none: the plain area only)'}" +
          (f": the image's room {room[1]:#x}..{HB_AREA_HI:#x}, chip key {room[0]:#06x}" if room else ""))
    for fid, m in json.loads((mdir / "modules.json").read_text()).items():
        if m.get("kind") in ("effect", "pack", "bluetooth"):   # (not in the list of synths: nothing to pick)
            continue
        entry = {"script": f"{fid}.txt"}
        name = m.get("module") or m.get("title")
        if not m.get("packaged"):
            dock = dock_of(fid, m, mdir, fw)
            if not dock:
                print(f"{fid:10} no .dock here: no Try it")
                continue
            blob = dock.read_bytes()
            packs = [mdir / f for f in m.get("packs", [])]
            if not all(p.exists() for p in packs):
                print(f"{fid:10} its data pack is not here: no Try it")
                continue
            try:
                secs = sectors_for(blob, [p.read_bytes() for p in packs], room)
            except ValueError as e:
                print(f"{fid:10} {e}: no Try it")
                continue
            name = hbmod.parse(blob)[0]["name"]
            (out / f"{fid}.flash").write_bytes(sector_file(secs))
            entry["flash"] = f"{fid}.flash"
            if packs:
                entry["packs"] = [p.name for p in packs]
        (out / f"{fid}.txt").write_text(script(name, bool(m.get("builtin"))))
        entry["module"] = name
        index["modules"][fid] = entry
        print(f"{fid:10} {name:12} {'flash + ' if 'flash' in entry else ''}script")
    (out / "index.json").write_text(json.dumps(index, indent=1) + "\n")


if __name__ == "__main__":
    main(sys.argv[1:])
