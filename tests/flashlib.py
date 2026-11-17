# SPDX-License-Identifier: GPL-3.0-only
"""fm1-dock's flash as fm1-live --flash keeps it (1 MiB, as stored), for the tests: the deployed table, the add-ons in
the plain areas and in the image's room (SFC-encrypted with the chip key below 0x93000, firmware/src/dock.c room),
and modules put there as dock.c's d_commit would."""
import struct
import sys
import zlib
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "tools"))
import fm1pkg_make  # noqa: E402
import hbmod  # noqa: E402

KEY = fm1pkg_make.KEY                           # the chip key of every FM-1 so far (the packages' 0x980F)
DT = (0x93000, 0x94000)
PLAIN = 0x93000                                 # the plain window's start (the FM-1's): below it, the SFC
SLOT, BOOT, DT_MAGIC = 0x544F4C53, 0x544F4F42, 0x54444248


def xip(off):
    return 0x02000000 + off - 0x4000


def sfc(buf, off):
    """buf, flash from off (32-byte aligned), through the SFC cipher in place (its own inverse)"""
    fm1pkg_make.sfc(buf, 0, len(buf), 0x4000 - off, KEY)


def read(flash, off, n):
    """flash [off, off + n) as XIP reads it: decrypted below the plain window"""
    b = bytearray(flash[off:off + n])
    if off < PLAIN:
        sfc(b, off)
    return bytes(b)


def entries(flash):
    """the table in force (the valid copy with the higher sequence number): [(magic, flash offset, size, crc)]"""
    best = None
    for at in DT:
        magic, seq, n, crc = struct.unpack_from("<4I", flash, at)
        if magic != DT_MAGIC or n > 32:
            continue
        ent = flash[at + 16:at + 16 + 16 * n]
        if zlib.crc32(struct.pack("<II", seq, n) + ent) != crc:
            continue
        if best is None or (seq - best[0]) & 0xFFFFFFFF < 0x80000000:
            best = (seq, ent)
    if not best:
        return []
    return [(m, x - 0x02000000 + 0x4000, s, c) for m, x, s, c in struct.iter_unpack("<4I", best[1])]


def table(flash):
    """the deployed modules: [(name, version, flash offset, checks out)], each read as fm1-dock reads it"""
    out = []
    for magic, off, size, crc in entries(flash):
        if magic != SLOT:
            out.append(("BOOT?", 0, off, False))
            continue
        blob = read(flash, off, size)
        try:
            h, relocs, image = hbmod.parse(blob)
        except Exception:
            out.append(("?", 0, off, False))
            continue
        ok = hbmod.crc_of(blob[:76], relocs, image) == h["crc"] == crc
        out.append((h["name"], h["version"], off, ok))
    return out


def put(flash, blob, off, size=None):
    """blob (a .dock) relocated to run at flash off and written there (encrypted below the plain window), as
    d_commit leaves it; -> its table entry (size: the entry's, to make it take more room than it is)"""
    moved = bytearray(hbmod.move(blob, xip(off)))
    crc = hbmod.parse(bytes(moved))[0]["crc"]
    n = len(moved)
    flash[off:off + ((n + 0xFFF) & ~0xFFF)] = b"\xff" * ((n + 0xFFF) & ~0xFFF)
    if off < PLAIN:
        sfc(moved, off)
    flash[off:off + n] = moved
    return (SLOT, xip(off), size or n, crc)


def write_table(flash, slots, seq=1):
    """slots into DT_A as the table in force (seq), DT_B erased"""
    body = b"".join(struct.pack("<4I", *s) for s in slots)
    head = struct.pack("<4I", DT_MAGIC, seq, len(slots), zlib.crc32(struct.pack("<2I", seq, len(slots)) + body))
    flash[DT[0]:DT[0] + 0x1000] = (head + body).ljust(0x1000, b"\xff")
    flash[DT[1]:DT[1] + 0x1000] = b"\xff" * 0x1000


def variant(sine, name, version):
    """the sine module under another name / version (its CRC made again)"""
    h, relocs, image = hbmod.parse(sine)
    h["name"], h["version"] = name, version
    return hbmod.pack(h, relocs, image)
