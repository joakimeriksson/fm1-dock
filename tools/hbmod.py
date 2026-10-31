#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build a fm1-dock module (firmware/dock/hb_module.h) into a .dock file, and move one.

  tools/hbmod.py modules/sine [modules/sid ...]     -> build/modules/<dir>.dock
  tools/hbmod.py --info build/modules/sine.dock
  tools/hbmod.py probes/timing.c                     -> build/probes/timing.dock (tools/probe.py)

A module is a directory of C / C++ files (*.c, *.cc), and module.cfg: `name =`, `version =`,
`cflags =`, `cflags FILE =` more for one of its files (after cflags; FILE as in `sources`, or a file of the
directory: DUB-1's engine -O2, its UI -Os, as DUB-1 builds them), `cxxflags =` (C++ only), `sources =` for files
elsewhere (relative to the directory),
`includes =` directories for -I (relative to the directory; a module may live outside fm1-dock's tree, as
FM-1 SID's fm1-dock/ does in its own repository),
`libs =` toolchain libraries (`m c` for libm and libc), `kind = probe` for a probe (hb_module.h,
"Probes": probes/probe.ld, flag HB_F_PROBE; a single .c file is a probe by itself). Every module also gets
modules/lib/hb_lib.c (memcpy, memset, ...) and the toolchain's compiler_rt (division, 64-bit and
soft-float helpers): it links against nothing of the host's.

Relocation: the module is linked twice (modules/module.ld at BASE_A and at BASE_B) and the two
images compared. A word that differs by exactly BASE_B - BASE_A holds a module address: it is
a relocation. Any other difference (an address split over two instructions, a layout that
depends on the base) fails the build. BASE_B - BASE_A has a zero low byte and a non-zero second
byte, so every relocated word starts one byte before its first differing byte.

File layout (and slot layout in flash): hb_file_t (80 B), n_relocs u32 offsets, 0xFF up to the
next 256-byte boundary, the image. `base` is where the image runs; move() relocates it.
"""
import argparse
import re
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build  # noqa: E402  (the toolchain runner, tc / tc_all)

SRC = build.SRC
MODS = SRC / "modules"
OUTDIR = SRC / "build" / "modules"
BASE_A = 0x02200000
BASE_B = 0x02211100
DELTA = BASE_B - BASE_A
HB_MAGIC = 0x52425248
HB_ABI = 2
HDR = struct.Struct("<III24sIIIIIIIIIII")      # hb_file_t, 80 bytes
assert HDR.size == 80
MOD_RAM, MOD_RAM_SIZE = 0x01C30000, 0x44000
MOD_RT, MOD_RT_SIZE = 0x01C04000, 0x4000
PROBE_RAM, PROBE_RAM_SIZE, PROBE_FILE = 0x01C60000, 0x14000, 0x30000
HB_F_PROBE = 0x100
PROBES = SRC / "probes"
CFLAGS = ["-O2", "-ffunction-sections", "-fdata-sections", "-fno-builtin", "-ffreestanding", "-Wall",
          "-Wno-unused-function", "-Ifirmware/dock", "-Imodules/lib"]
FIELDS = ("magic", "abi", "flags", "name", "version", "code_size", "data_size", "bss_size", "rt_size",
          "worst_cycles", "desc", "base", "n_relocs", "reserved", "crc")


def image_offset(n_relocs):
    return (HDR.size + 4 * n_relocs + 255) & ~255


def parse(blob):
    h = dict(zip(FIELDS, HDR.unpack_from(blob)))
    h["name"] = h["name"].split(b"\0")[0].decode()
    if h["magic"] != HB_MAGIC:
        raise ValueError("not a fm1-dock module (magic)")
    io = image_offset(h["n_relocs"])
    relocs = list(struct.unpack_from(f"<{h['n_relocs']}I", blob, HDR.size))
    size = h["code_size"] + h["data_size"] + h["rt_size"]
    image = blob[io:io + size]
    if len(image) != size:
        raise ValueError("truncated")
    return h, relocs, image


def crc_of(head76, relocs, image):
    return zlib.crc32(head76 + struct.pack(f"<{len(relocs)}I", *relocs) + image) & 0xFFFFFFFF


def pack(h, relocs, image):
    vals = [h[f] if f != "name" else h["name"].encode()[:23] for f in FIELDS]
    vals[-1] = 0
    head = HDR.pack(*vals)
    h["crc"] = crc_of(head[:76], relocs, image)
    head = head[:76] + struct.pack("<I", h["crc"])
    body = head + struct.pack(f"<{len(relocs)}I", *relocs)
    return body + b"\xff" * (image_offset(len(relocs)) - len(body)) + image


def move(blob, at):
    """the module, its header at XIP address `at`, relocated to run there"""
    h, relocs, image = parse(blob)
    new_base = at + image_offset(len(relocs))
    d = (new_base - h["base"]) & 0xFFFFFFFF
    img = bytearray(image)
    for o in relocs:
        (w,) = struct.unpack_from("<I", img, o)
        struct.pack_into("<I", img, o, (w + d) & 0xFFFFFFFF)
    h["base"] = new_base
    return pack(h, relocs, bytes(img))


def find_relocs(a, b):
    if len(a) != len(b):
        raise SystemExit(f"hbmod: the two links differ in size ({len(a)} / {len(b)}): the layout depends on the base")
    relocs, i = [], 0
    while i < len(a):
        if a[i] == b[i]:
            i += 1
            continue
        s = i - 1                                # DELTA's low byte is 0: the word starts a byte before
        wa = struct.unpack_from("<I", a, s)[0] if s >= 0 and s + 4 <= len(a) else None
        wb = struct.unpack_from("<I", b, s)[0] if wa is not None else None
        if wa is None or (wb - wa) & 0xFFFFFFFF != DELTA:
            ctx = a[max(0, i - 8):i + 8].hex()
            raise SystemExit(f"hbmod: image offset {i:#x} changes with the base, but not by {DELTA:#x} "
                             f"(an address split over instructions?): {ctx}")
        relocs.append(s)
        i = s + 4
    return relocs


def read_cfg(d):
    cfg = {"name": d.name.upper(), "version": "1", "cflags": "", "cxxflags": "", "sources": "", "includes": "", "libs": "",
           "kind": "module"}
    if d.is_file():                             # a single .c: a probe (its own flags: /* hbmod-cflags: ... */)
        cfg.update(name=d.stem.upper(), kind="probe")
        m = re.search(r"hbmod-cflags:\s*(.+?)\s*\*/", d.read_text())
        if m:
            cfg["cflags"] = m.group(1)
        return cfg
    p = d / "module.cfg"
    if p.exists():
        for ln in p.read_text().splitlines():
            ln = ln.split("#", 1)[0].strip()
            if "=" in ln:
                k, v = (x.strip() for x in ln.split("=", 1))
                cfg[k] = v
    return cfg


def symbols(syms):
    return {ln.split()[-1]: int(ln.split()[0], 16) for ln in syms.splitlines()
            if re.match(r"^[0-9a-f]{8} ", ln) and len(ln.split()) >= 3}


def link(objs, out, base, libs=(), probe=False):
    ld = out / f"module_{base:08x}.ld"
    ld.write_text((PROBES / "probe.ld" if probe else MODS / "module.ld").read_text().replace("@BASE@", f"{base:#010x}"))
    elf = out / f"module_{base:08x}.elf"
    tc_root = build.toolchain()
    paths = []
    for name in (*libs, "compiler_rt"):
        rel = f"pi32v2/lib/lib{name}.a"
        if not (tc_root / rel).exists():
            raise SystemExit(f"hbmod: no {rel} in the toolchain")
        # (the toolchain is mounted at /opt/jieli in Docker: pass it by that path or the host's)
        paths.append(f"/opt/jieli/{rel}" if build.use_docker() else str(tc_root / rel))
    build.tc("pi32v2/bin/ld", "--gc-sections", "-e", "hb_module", "-T", ld, *objs,
             "--start-group", *paths, "--end-group", "-o", elf)
    names = [f"{s}.{base:08x}.bin" for s in ("text", "data", "rt")]
    for n in names:
        (out / n).unlink(missing_ok=True)
    _, _, _, syms = build.tc_all(
        ("common/bin/objcopy", "-O", "binary", "-j", ".text", elf, out / names[0]),
        ("common/bin/objcopy", "-O", "binary", "-j", ".data", elf, out / names[1]),
        ("common/bin/objcopy", "-O", "binary", "-j", ".ram_text", elf, out / names[2]),
        ("common/bin/objdump", "-t", elf))
    sym = symbols(syms)
    parts = [(out / n).read_bytes() if (out / n).exists() else b"" for n in names]
    if len(parts[0]) != sym["_hb_text_end"] - base:
        raise SystemExit("hbmod: .text is not where module.ld puts it")
    img = bytearray(parts[0])
    for name, blob in (("_hb_data_load", parts[1]), ("_hb_rt_load", parts[2])):
        if blob:
            lma = sym[name] - base
            if lma < len(img):
                raise SystemExit(f"hbmod: {name} overlaps the image")
            img += b"\xff" * (lma - len(img)) + blob
    if "hb_module" not in sym:
        raise SystemExit("hbmod: no hb_module descriptor (HB_DESC)")
    return elf, bytes(img), sym, syms, sym["hb_module"] - base, [len(p) for p in parts]


def build_module(d):
    d = Path(d).resolve()
    cfg = read_cfg(d)
    probe = cfg["kind"] == "probe"
    outdir = SRC / "build" / "probes" if probe else OUTDIR
    out = outdir / (d.stem if d.is_file() else d.name)
    out.mkdir(parents=True, exist_ok=True)
    srcs = [d] if d.is_file() else sorted([*d.glob("*.c"), *d.glob("*.cc")])
    base_dir = d.parent if d.is_file() else d
    srcs = srcs + [(base_dir / s).resolve() for s in cfg["sources"].split()] + [MODS / "lib" / "hb_lib.c"]
    inc_dir = base_dir.relative_to(SRC) if base_dir.is_relative_to(SRC) else base_dir
    flags = [*CFLAGS, f"-I{inc_dir}", *[f"-I{(base_dir / i).resolve()}" for i in cfg["includes"].split()], "-Iprobes",
             *cfg["cflags"].split()]
    if cfg["libs"].split():                     # libc / libm: their headers too
        inc = "/opt/jieli/pi32v2/include" if build.use_docker() else str(build.toolchain() / "pi32v2/include")
        flags += ["-isystem", inc]
    # (object names from the path below the module, so resid/sid.cc and sid.c do not collide; a file
    # outside it by its path from the nearest directory above the module that holds it: ../src/sid.c
    # and ../ext/resid-0.16/sid.cc neither, nor ../../src/sid.c from a module one level deeper)
    def oname(s):
        if s.is_relative_to(base_dir):
            return "_".join(s.relative_to(base_dir).with_suffix("").parts)
        for up, anc in enumerate(base_dir.parents, 1):
            if s.is_relative_to(anc):
                return "up" * up + "_" + "_".join(s.relative_to(anc).with_suffix("").parts).replace(".", "_")
        return s.stem
    objs = [out / (oname(s) + ".o") for s in srcs]
    own = {(base_dir / k.split(None, 1)[1]).resolve(): v.split() for k, v in cfg.items() if k.startswith("cflags ")}
    for f in own:
        if f not in srcs:
            raise SystemExit(f"hbmod: module.cfg: cflags for {f}, not one of the module's files")
    build.tc_all(*[("cxx", *flags, *cfg["cxxflags"].split(), *own.get(s, []), "-c", s, "-o", o) if s.suffix == ".cc"
                   else ("cc", *flags, *own.get(s, []), "-c", s, "-o", o) for s, o in zip(srcs, objs)])
    libs = cfg["libs"].split()
    elf_a, img_a, sym, syms, desc, (code, data, rt) = link(objs, out, BASE_A, libs, probe)
    _, img_b, _, _, desc_b, _ = link(objs, out, BASE_B, libs, probe)
    if any(n in sym for n in ("__init_array_start", "__ctors_start")) or ".init_array" in build.tc(
            "common/bin/objdump", "-h", elf_a):
        raise SystemExit("hbmod: static constructors (.init_array): a module has nobody to run them; "
                         "construct in init() (placement new)")
    relocs = find_relocs(img_a, img_b)
    errors = []
    if desc != 0 or desc_b != 0:
        errors.append(f"the descriptor is at {desc:#x}, not at the start of the image")
    ram, ram_size = (PROBE_RAM, PROBE_RAM_SIZE) if probe else (MOD_RAM, MOD_RAM_SIZE)
    if sym["_hb_data_start"] != ram:
        errors.append(".data does not start the module RAM")
    if sym["_hb_rt_start"] != MOD_RT:
        errors.append(".ram_text does not start the module RAM text")
    ram_end = sym["_hb_bss_end"]
    if ram_end - ram > ram_size:
        errors.append(f"RAM {ram_end - ram} B > {ram_size}")
    if probe and image_offset(len(relocs)) + len(img_a) > PROBE_FILE:
        errors.append(f"a probe's file may be {PROBE_FILE} B")
    if rt > MOD_RT_SIZE:
        errors.append(f".ram_text {rt} B > {MOD_RT_SIZE}")
    dis = build.tc("common/bin/objdump", "-d", elf_a)
    (out / "module.dis").write_text(dis)
    for ln in dis.splitlines():                 # as the host: nothing may call or load the chip ROM
        mm = build.LINE.match(ln)
        if mm:
            for v in re.findall(r"= (-?\d+) <", mm.group(3)):
                if 0xFFC00000 <= (int(v) & 0xFFFFFFFF) < 0xFFD00000:
                    errors.append(f"reference to ROM address {int(v) & 0xFFFFFFFF:#010x}")
    for e in errors:
        print(f"  FAIL  {d.name}: {e}")
    if errors:
        raise SystemExit("hbmod: checks failed")
    # (RAM addresses are fixed, HB_MOD_RAM: only flash addresses are relocated, in .data's image too)
    h = {"magic": HB_MAGIC, "abi": HB_ABI, "flags": HB_F_PROBE if probe else 0, "name": cfg["name"],
         "version": int(cfg["version"]), "code_size": code, "data_size": data, "bss_size": ram_end - ram - data,
         "rt_size": rt, "worst_cycles": 0, "desc": desc, "base": BASE_A, "n_relocs": len(relocs), "reserved": 0, "crc": 0}
    blob = pack(h, relocs, img_a)
    path = outdir / f"{out.name}.dock"
    path.write_bytes(blob)
    print(f"{cfg['kind']} {cfg['name']:<10} {path.relative_to(SRC)}: code {code} B, data {data} B, bss {h['bss_size']} B, "
          f"ram_text {rt} B, {len(relocs)} relocs, crc {h['crc']:08x}")
    return path


def info(path):
    blob = Path(path).read_bytes()
    h, relocs, image = parse(blob)
    ok = crc_of(blob[:76], relocs, image) == h["crc"]
    print(f"{path}: {h['name']} v{h['version']} abi {h['abi']}, base {h['base']:#010x}, code {h['code_size']} B, "
          f"data {h['data_size']} B, bss {h['bss_size']} B, ram_text {h['rt_size']} B, {len(relocs)} relocs, "
          f"crc {h['crc']:08x} {'ok' if ok else 'BAD'}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="*")
    ap.add_argument("--info", nargs="+")
    a = ap.parse_args()
    for p in a.info or []:
        info(p)
    for d in a.dirs:
        build_module(d)


if __name__ == "__main__":
    main()
