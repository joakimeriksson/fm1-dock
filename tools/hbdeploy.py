#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""fm1-dock's deploy protocol on the computer: a module file (.dock, tools/hbmod.py) into a running
fm1-dock over SysEx, and the other requests (the module list, delete, select, the sine, the log).
The device side and the frame format: firmware/src/dock.c, "deploys".

  frame    F0 7D 48 42 cmd seq pack7(payload + CRC-32 of the payload, little endian) F7
  replies  (USB only) cmd | 0x40, the same seq; the payload starts with a status byte

Over USB every frame is answered, and a chunk that gets no answer is sent again. Over the DIN jack
(or a .syx file) nothing comes back: din_script() / the --blind sender leave the device time where
it needs it (after BEGIN: the module stops; after COMMIT: the flash is written).

The USB transport uses python-rtmidi (`uv run --with python-rtmidi tools/dock.py ...`)."""
import struct
import time
import zlib

MAN = bytes([0x7D, 0x48, 0x42])                 # 7D: non-commercial; "HB"
HELLO, LIST, BEGIN, DATA, COMMIT, ABORT, DELETE, LOG, SAFE, SELECT = 0x01, 0x02, 0x10, 0x11, 0x12, 0x13, 0x14, 0x20, 0x21, 0x22
RUN, RESULT = 0x30, 0x31                       # probes (tools/probe.py): RUN in place of COMMIT, from RAM
INFO = 0x03                                    # the unit and the build (dock.c info_fill; fm1-dock after 0.8)
MODULE = 0x23                                  # a payload for the module playing (its data(): FM-1 SID takes a tune)
BANKS, BANK_READ, BANK_WRITE, BANK_ERASE = 0x24, 0x25, 0x26, 0x27   # the modules' banks (their presets), below
STATUS = ["done", "bad CRC", "bad header", "too big", "no space", "flash error", "verify failed", "data missing",
          "not started", "not found", "bad message", "refused"]
STATE = ["ok", "bad CRC", "bad header", "too big", "crashed", "init failed", "too slow"]
KIND = ["built in", "image", "deployed", "boot", "effect", "effect (image)"]   # (4, 5: effects, their places 0x80 up)
CHUNK = 256
BYTE_MS = 10 * 1000 / 31250                     # a MIDI byte on the DIN jack: 0.32 ms
HRBR_MAGIC = 0x52425248                         # "HRBR"


def pack7(b):
    """8-to-7 bits, LSB first (ota.c's ota_pack7)"""
    out, acc, nb = bytearray(), 0, 0
    for x in b:
        acc |= x << nb
        nb += 8
        while nb >= 7:
            out.append(acc & 0x7F)
            acc >>= 7
            nb -= 7
    if nb:
        out.append(acc & 0x7F)
    return bytes(out)


def unpack7(b):
    out, acc, nb = bytearray(), 0, 0
    for x in b:
        acc |= (x & 0x7F) << nb
        nb += 7
        if nb >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nb -= 8
    return bytes(out)


def frame(cmd, seq, payload=b""):
    body = payload + struct.pack("<I", zlib.crc32(payload))
    return bytes([0xF0]) + MAN + bytes([cmd, seq & 0x7F]) + pack7(body) + bytes([0xF7])


def parse(msg):
    """a reply: (cmd without 0x40, seq, payload), or None if it is not fm1-dock's or is damaged"""
    msg = bytes(msg)
    if len(msg) < 7 or msg[0] != 0xF0 or msg[-1] != 0xF7 or msg[1:4] != MAN:
        return None
    d = unpack7(msg[6:-1])
    if len(d) < 4 or zlib.crc32(d[:-4]) != struct.unpack_from("<I", d, len(d) - 4)[0]:
        return None
    return msg[4] & ~0x40, msg[5], d[:-4]


def name24(s):
    b = s.encode("latin-1")[:23]
    return b + b"\0" * (24 - len(b))


def module_info(blob):
    """name, version and size from a .dock's header; refuses what is not one"""
    if len(blob) < 80 or struct.unpack_from("<I", blob)[0] != HRBR_MAGIC:
        raise SystemExit("not a fm1-dock module file (.dock)")
    name = blob[12:36].split(b"\0")[0].decode("latin-1")
    (version,) = struct.unpack_from("<I", blob, 36)
    return name, version


def deploy_frames(blob, last=None):
    """(cmd, payload) for BEGIN, every DATA chunk, COMMIT (or `last`: a probe's RUN)"""
    name, _ = module_info(blob)
    out = [(BEGIN, struct.pack("<II", len(blob), zlib.crc32(blob)) + name24(name))]
    for off in range(0, len(blob), CHUNK):
        out.append((DATA, struct.pack("<I", off) + blob[off:off + CHUNK]))
    out.append(last or (COMMIT, b""))
    return out


def run_payload(run_id, arg=b""):
    return struct.pack("<I", run_id) + arg


def din_script(blob, t0, settle=400, write_ms=None, last=None):
    """the deploy as fm1-live FM1_BENCH_KEYS MIDI entries (`ms:mHEX`), paced for a device that cannot
    answer: BEGIN at t0, the data `settle` ms later (back to back: the jack's byte time paces them),
    COMMIT after. Returns (entries, ms the COMMIT has arrived by)."""
    out, t = [], t0
    for i, (cmd, payload) in enumerate(deploy_frames(blob, last)):
        f = frame(cmd, i, payload)
        if cmd != DATA:
            t += settle if cmd == COMMIT else 0
        out.append(f"{int(t)}:m{f.hex()}")
        t += len(f) * BYTE_MS
        if cmd == BEGIN:
            t += settle
    return out, t


class Port:
    """a USB-MIDI port pair by part of its name (the device calls itself Felucca)"""

    def __init__(self, name=None, blind=False):
        try:
            import rtmidi
        except ImportError:
            raise SystemExit("needs python-rtmidi: uv run --with python-rtmidi tools/dock.py ...")
        self.out = rtmidi.MidiOut()
        self.inp = None if blind else rtmidi.MidiIn()
        want = self.want = [name] if name else ["Felucca", "FM1-DOCK", "HARBOR", "FM-1"]
        self.out.open_port(self._find(self.out, want))
        if self.inp:
            self.inp.open_port(self._find(self.inp, want))
            self.inp.ignore_types(sysex=False, timing=True, active_sense=True)
        self.seq = 0
        self.blind = blind

    @staticmethod
    def _find(dev, want):
        ports = dev.get_ports()
        for w in want:
            for i, p in enumerate(ports):
                if w.lower() in p.lower():
                    return i
        raise SystemExit(f"no MIDI port matching {' / '.join(want)}; there are: {', '.join(ports) or 'none'}")

    def wait_back(self, timeout=12.0):
        """the unit restarted (its USB went away): its ports found again and HELLO answered, or SystemExit"""
        import rtmidi
        want = self.want
        for port in (self.out, self.inp):       # (Windows: only one program, and one handle, has a port)
            try:
                port.close_port()
            except Exception:
                pass
        end = time.monotonic() + timeout
        time.sleep(1.0)
        while time.monotonic() < end:
            try:
                out, inp = rtmidi.MidiOut(), rtmidi.MidiIn()
                out.open_port(self._find(out, want))
                inp.open_port(self._find(inp, want))
                inp.ignore_types(sysex=False, timing=True, active_sense=True)
                self.out, self.inp = out, inp
                self.ask(HELLO, timeout=0.5, tries=2)
                return
            except (SystemExit, Exception):     # (not there yet, or not openable yet)
                time.sleep(0.5)
        raise SystemExit("the FM-1 did not come back after its restart")

    def send(self, cmd, payload=b""):
        self.seq = (self.seq + 1) & 0x7F
        f = frame(cmd, self.seq, payload)
        self.out.send_message(list(f))
        if self.blind:
            time.sleep(len(f) * BYTE_MS / 1000 * 1.2)
        return self.seq

    def replies(self, seq, timeout):
        """the replies to seq until timeout (s) passes without one"""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            m = self.inp.get_message()
            if not m:
                time.sleep(0.0005)
                continue
            r = parse(m[0])
            if r and r[1] == seq:
                end = time.monotonic() + timeout
                yield r[0], r[2]

    def ask(self, cmd, payload=b"", timeout=1.0, tries=3):
        """one request, its (first) reply's payload"""
        for _ in range(tries):
            seq = self.send(cmd, payload)
            for c, d in self.replies(seq, timeout):
                if c == cmd or c == 0x3F:
                    return d
        raise SystemExit(f"no answer from the FM-1 (cmd {cmd:#04x}): is fm1-dock running, the port right?")


def send_file(p, blob, arg=0, chunk=CHUNK):
    """a file to the module playing, in MODULE frames (hb_module.h, "a file to a module"): B [size][arg],
    D [at][bytes] in order, E. The host writes nothing to flash; the module takes it as it
    arrives (FM6 and PROPHET keep it in their banks).
    Returns None when it was taken, else (the step, the reply)"""
    steps = [b"B" + struct.pack("<II", len(blob), arg)]
    steps += [b"D" + struct.pack("<I", at) + blob[at:at + chunk] for at in range(0, len(blob), chunk)]
    steps.append(b"E")
    for st in steps:                            # (the end may write flash: asked once, with time)
        d = p.ask(MODULE, st, timeout=10.0, tries=1) if st == b"E" else p.ask(MODULE, st)
        if not d or d[0] != 0:
            return chr(st[0]), d
    return None


def status(d):
    s = d[0] if d else 10
    return STATUS[s] if s < len(STATUS) else f"status {s}"


CPU1 = ("off", "did not start", "failed: wrong or late (self-test, screening or a job)", "2 cores", "held after a misread",
        "being screened", "off since a crash with 2 cores, until power-off", "held for Bluetooth, until power-off",
        "not started: the supplies not as stock")


def hello(p):
    d = p.ask(HELLO)
    proto, abi, n, cur, rx = d[1], d[2], d[3], d[4], d[5]
    big, total, max_file, crash_pc, crash_mod, worst, over, log_w, block = struct.unpack_from("<9I", d, 8)
    h = dict(proto=proto, abi=abi, n=n, cur=cur, receiving=rx, fx=d[6], largest=big, free=total, max_file=max_file,
             crash_pc=crash_pc, crash_module=crash_mod, worst=worst, overruns=over, log_w=log_w, block=block)
    if len(d) >= 60:                            # the second core (firmware/src/cpu1.c): its state, a job's time
        h.update(cpu1=CPU1[d[44]] if d[44] < len(CPU1) else d[44], cpu1_timeouts=d[45],
                 cpu1_job=struct.unpack_from("<I", d, 48)[0], cpu1_worst=struct.unpack_from("<I", d, 52)[0],
                 cpu1_stack_free=struct.unpack_from("<I", d, 56)[0])
    if len(d) >= 68:                            # every add-on area's bytes, the image's end (its room from there)
        h.update(room=struct.unpack_from("<I", d, 60)[0], image_end=struct.unpack_from("<I", d, 64)[0])
    return h


def info(p):
    """INFO's record (format 1, dock.c info_fill), or None from an fm1-dock without it (it answers bad message)"""
    d = p.ask(INFO)
    if len(d) < 88 or d[0] != 0 or d[1] < 1:
        return None
    s = lambda o, n: d[o:o + n].split(b"\0")[0].decode("latin-1")
    flash, free, big, ram, used, mhz = struct.unpack_from("<6I", d, 12)
    return dict(format=d[1], board=("FM-1", "SMK-37")[d[2]] if d[2] < 2 else d[2], cpu1_state=d[3],
                cpu1=CPU1[d[3]] if d[3] < len(CPU1) else d[3], cores=2 if d[3] in (3, 5) else 1, reset=d[4],
                ble_addr=":".join(f"{x:02X}" for x in reversed(d[6:12])) if d[5] else None, flash_id=flash,
                flash_size=1 << (flash & 0xFF) if 16 <= flash & 0xFF <= 26 else None, free=free, largest=big,
                mod_ram=ram, mod_ram_used=used, mhz=mhz, soc=s(36, 8), version=s(44, 16), build=s(60, 12),
                model=s(72, 16), **(dict(room=struct.unpack_from("<I", d, 88)[0],
                                          image_end=struct.unpack_from("<I", d, 92)[0]) if len(d) >= 96 else {}))


def modules(p):
    seq = p.send(LIST)
    out = []
    for c, d in p.replies(seq, 1.0):
        if c != LIST:
            continue
        if d[1] == 0xFF:
            break
        version, size, at = struct.unpack_from("<III", d, 4)
        out.append(dict(index=d[1], state=STATE[d[2]] if d[2] < len(STATE) else d[2], kind=KIND[d[3]] if d[3] < len(KIND) else d[3],
                        version=version, size=size, at=at, name=d[16:40].split(b"\0")[0].decode("latin-1")))
    return out


def deploy(p, blob, say=print):
    name, version = module_info(blob)
    if not p.blind:
        h = hello(p)
        if h["abi"] != struct.unpack_from("<I", blob, 4)[0]:
            raise SystemExit(f"the FM-1's fm1-dock speaks ABI {h['abi']}, {name} was built for "
                             f"{struct.unpack_from('<I', blob, 4)[0]}: update one of them")
        if struct.unpack_from("<I", blob, 8)[0] & 0x800 and not h.get("fx"):
            raise SystemExit(f"{name} is an effect: this fm1-dock takes none yet (1.0 and later)")
        if len(blob) > h["max_file"]:
            raise SystemExit(f"{name}: {len(blob)} B, fm1-dock takes up to {h['max_file']} B")
        say(f"{name} v{version}, {len(blob)} B; the FM-1 has {h['free'] // 1024} KiB free "
            f"(largest gap {h['largest'] // 1024} KiB)")
    t0 = time.monotonic()
    frames = deploy_frames(blob)
    for i, (cmd, payload) in enumerate(frames):
        if cmd == COMMIT and not p.blind:
            log_at = hello(p)["log_w"]
        if p.blind:
            p.send(cmd, payload)
            if cmd == BEGIN:
                time.sleep(0.4)
            if cmd == DATA and i % 32 == 0:
                say(f"  {min(len(blob), (i - 1) * CHUNK + CHUNK) * 100 // len(blob):3d} %")
            continue
        try:
            d = p.ask(cmd, payload, timeout=30.0 if cmd == COMMIT else 2.0, tries=1 if cmd == COMMIT else 4)
        except SystemExit:
            if cmd != COMMIT:
                raise
            p.wait_back()                       # (its answer was lost: the unit's log says what came of it)
            if "deploy: done" not in log_text(p, log_at)[0]:
                raise
            say(f"{name}: in flash, its answer was lost ({time.monotonic() - t0:.1f} s)")
            return
        if d[0] != 0:
            if cmd != COMMIT:
                p.send(ABORT)
            raise SystemExit(f"{name}: {status(d)} (at {['BEGIN', 'DATA', 'COMMIT'][(cmd > BEGIN) + (cmd == COMMIT)]})")
        if cmd == DATA and (i % 64 == 0 or i == len(frames) - 2):
            say(f"  {min(len(blob), i * CHUNK) * 100 // len(blob):3d} %")
        if cmd == COMMIT and len(d) > 2 and d[2] == 1:   # written where code ran since power-on: run after a restart
            say(f"{name}: in flash; the FM-1 restarts to run it")
            p.wait_back()
            say(f"{name}: running ({time.monotonic() - t0:.1f} s)")
        elif cmd == COMMIT:
            (at,) = struct.unpack_from("<I", d, 4)
            say(f"{name}: in flash at {at:#010x}, running ({time.monotonic() - t0:.1f} s)")
    if p.blind:
        time.sleep(3.0)
        say(f"{name}: sent (blind: check the FM-1's screen)")


def log_text(p, since=0):
    """the log (its last 2 KiB, or from the byte count since on) and HELLO"""
    h = hello(p)
    w, frm, out = h["log_w"], max(since, h["log_w"] - 2048, 0), bytearray()
    while frm < w:
        d = p.ask(LOG, struct.pack("<I", frm))
        _, at = struct.unpack_from("<II", d, 4)
        chunk = d[12:]
        if not chunk:
            break
        out += chunk
        frm = at + len(chunk)
    return out.decode("latin-1"), h


# ---- the modules' banks (their user presets): listed, read, written and given back over USB, and kept in a .hbk
# file: a backup of a unit, or one synth's presets for another unit. The host carries the bytes as they are (every
# module's bank is its own format); a bank is the module's whose name it carries, so a file lands on the same synth
# on any unit. dock.c, "the banks from the computer".
BANK_MAX = 8176
HBK_MAGIC = b"HBKF"


def banks(p):
    """the banks in use: ([dict(slot, name: None for a bank of no module on the unit, idx, key, size, crc)],
    dict(slots, free)); stops at an fm1-dock without the commands"""
    seq = p.send(BANKS)
    out, end = [], None
    for c, d in p.replies(seq, 1.0):
        if c != BANKS:
            if c == 0x3F:
                break
            continue
        if d[0] != 0 or len(d) < 4:
            break
        if d[1] == 0xFF:
            end = dict(slots=d[2], free=d[3])
            break
        key, size, crc, idx = struct.unpack_from("<4I", d, 4)
        out.append(dict(slot=d[1], name=d[20:44].split(b"\0")[0].decode("latin-1") if d[2] != 0xFF else None,
                        idx=idx, key=key, size=size, crc=crc))
    if end is None:
        raise SystemExit("this fm1-dock has no bank commands (0.9 and before): update it first")
    return out, end


def bank_read(p, b):
    """bank b's bytes (a banks() entry), checked against its CRC"""
    out = bytearray()
    while len(out) < b["size"]:
        d = p.ask(BANK_READ, struct.pack("<B3xI", b["slot"], len(out)))
        if d[0] != 0 or len(d) <= 8 or struct.unpack_from("<I", d, 4)[0] != len(out):
            raise SystemExit(f"bank {b['slot']}: {status(d)}")
        out += d[8:]
    if len(out) != b["size"] or zlib.crc32(out) != b["crc"]:
        raise SystemExit(f"bank {b['slot']}: it changed while it was read (the synth saved?): try again")
    return bytes(out)


def bank_write_frames(name, idx, data):
    """BANK_WRITE's payloads for one bank: B, every D, E"""
    out = [b"B\0" + struct.pack("<HII", idx, len(data), zlib.crc32(data)) + name24(name)]
    out += [b"D\0\0\0" + struct.pack("<I", at) + data[at:at + CHUNK] for at in range(0, len(data), CHUNK)]
    return out + [b"E"]


def bank_write(p, name, idx, data):
    """one bank onto the unit; None when it is in flash, else (the step, the reply)"""
    for st in bank_write_frames(name, idx, data):
        d = p.ask(BANK_WRITE, st, timeout=5.0 if st[:1] == b"E" else 1.0, tries=1 if st[:1] == b"E" else 3)
        if not d or d[0] != 0:
            return chr(st[0]), d
    return None


def bank_erase(p, b):
    d = p.ask(BANK_ERASE, struct.pack("<B3xI", b["slot"], b["key"]), timeout=3.0, tries=1)
    return None if d and d[0] == 0 else d


def hbk_pack(entries):
    """a .hbk: "HBKF", u16 format 1, u16 count, 8 bytes 0; then per bank the module's name[24], u16 idx, u16 0,
    u32 the module's version (0: not known), u32 length, u32 CRC-32, the bytes. entries: dict(name, idx, version, data)"""
    out = bytearray(HBK_MAGIC + struct.pack("<HH8x", 1, len(entries)))
    for e in entries:
        out += name24(e["name"]) + struct.pack("<HHIII", e["idx"], 0, e.get("version", 0), len(e["data"]),
                                               zlib.crc32(e["data"])) + e["data"]
    return bytes(out)


def hbk_unpack(blob):
    """a .hbk's banks, each checked: [dict(name, idx, version, data)]"""
    if len(blob) < 16 or blob[:4] != HBK_MAGIC:
        raise SystemExit("not a preset file of fm1-dock (.hbk)")
    fmt, n = struct.unpack_from("<HH", blob, 4)
    if fmt != 1:
        raise SystemExit(f"a .hbk of format {fmt}: this tool reads format 1")
    out, at = [], 16
    for _ in range(n):
        if at + 40 > len(blob):
            raise SystemExit("the .hbk is cut short")
        idx, _, version, size, crc = struct.unpack_from("<HHIII", blob, at + 24)
        data = blob[at + 40:at + 40 + size]
        if not 0 < size <= BANK_MAX or len(data) != size or zlib.crc32(data) != crc:
            raise SystemExit("the .hbk is damaged (a bank's length or CRC)")
        out.append(dict(name=blob[at:at + 24].split(b"\0")[0].decode("latin-1"), idx=idx, version=version, data=data))
        at += 40 + size
    return out
