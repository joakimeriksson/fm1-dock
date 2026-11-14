#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""fm1-dock in fm1-emu (fm1-live, headless): the host and its modules, end to end.
  ./build.sh --no-package && tests/emu_test.py
Each case runs the firmware with a key / MIDI script and checks the sound and the host's log.
The slots are looked up by name in build/fm1-dock.bin's table, so the order may change."""
import os
import re
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "tools"))
import hbmod  # noqa: E402

OUT = SRC / "build" / "emu"
APP = SRC / "build" / "fm1-dock_xip.bin"
failures = []


def slots():
    """[(name, at)] from the slot table in the XIP image; the picker lists the built-in sine first"""
    img = APP.read_bytes()
    out = []
    for i in range(32):
        magic, at, size, crc = struct.unpack_from("<IIII", img, 0x30000 + 16 * i)
        if magic != 0x544F4C53:
            break
        h, _, _ = hbmod.parse(img[at - 0x02000000:at - 0x02000000 + size])
        out.append((h["name"], at, h))
    return out


def pick(name):
    """the key script that opens the picker and loads `name` (at 700..1150 ms)"""
    idx = 1 + [n for n, _, _ in slots()].index(name)
    return f"700:8,750:8^,900:e6:{idx},1100:8,1150:8^"


O_CC7, O_MASTER_CC7, O_CH, O_LOCAL, O_EQ, O_KNOBS, O_AT, O_THRU, O_ABOUT = range(9)   # the OPTIONS lines (dock.c), past the
# list's modules; then BLUETOOTH, the last
ACC = (180, 140, 255)                   # dock.c C_ACC, as the panel shows it (RGB565)


def acc_span(case, y0, y1):
    """the x span of the accent-coloured pixels in rows y0..y1 of the run's screen, (0, 0): none. The screen is
    fm1-live's panel picture's (1280 x 820, the LCD at 370, 42, two pixels a pixel: its panel.rs)"""
    ppm = OUT / case / "panel.ppm"
    if ppm.exists():                        # (P6 as fm1-live writes it: tools/dock.py keeps it without Pillow)
        raw = ppm.read_bytes()
        hdr = raw.split(b"\n", 3)
        w, px = int(hdr[1].split()[0]), hdr[3]
        at = lambda x, y: tuple(px[3 * (y * w + x):3 * (y * w + x) + 3])
    else:
        from PIL import Image
        img = Image.open(OUT / case / "panel.png").convert("RGB")
        at = lambda x, y: img.getpixel((x, y))
    xs = [x for y in range(y0, y1) for x in range(240)
          if all(abs(c - a) <= 12 for c, a in zip(at(370 + 2 * x, 42 + 2 * y), ACC))]
    return (min(xs), max(xs) + 1) if xs else (0, 0)


def options(sets, at=700, cur=0, hold=False):
    """the key script that opens the picker (module `cur` playing; hold: one with the panel, HOME held 1.5 s),
    goes down to each OPTIONS line o of sets [(o, n)], in order, presses SEL n times there, then HOME; and
    the time it ends at"""
    up = at + (1600 if hold else 50)
    n_mods, t = 1 + len(slots()), up + 150
    sel, out = cur, [f"{at}:8", f"{up}:8^"]
    for o, n in sets:
        out.append(f"{t}:e6:{n_mods + o - sel}")
        sel, t = n_mods + o, t + 100
        for _ in range(n):
            out += [f"{t}:3", f"{t + 50}:3^"]
            t += 100
    out += [f"{t}:8", f"{t + 50}:8^"]
    return ",".join(out), t + 50


def eq_keys(low=0, mid=0, high=0, on=False, at=700):
    """the key script that opens the EQ's page (OPTIONS, EQ; the sine playing), turns KNOB1..3 by the bands' dB
    (a band turned switches it on), SEL on its ON line with on, then HOME twice; and the time it ends at"""
    t = at + 200
    out = [f"{at}:8", f"{at + 50}:8^", f"{t}:e6:{1 + len(slots()) + O_EQ}", f"{t + 100}:3", f"{t + 150}:3^"]
    t += 300
    if on:
        out += [f"{t}:3", f"{t + 50}:3^"]
        t += 150
    for e, g in ((2, low), (3, mid), (4, high)):
        if g:
            out.append(f"{t}:e{e}:{g}")
            t += 100
    out += [f"{t}:8", f"{t + 50}:8^", f"{t + 150}:8", f"{t + 200}:8^"]
    return ",".join(out), t + 200


def usb_ask(frames, ms):
    """fm1-live headless with a computer on USB that sends frames [(at ms, bytes)]: fm1-dock's replies
    [(cmd, seq, payload)] (hbdeploy.parse)"""
    import dock
    import hbdeploy
    with tempfile.TemporaryDirectory() as wd:
        (Path(wd) / "work").mkdir()
        env = dict(os.environ, FM1_USB_SEND=";".join(f"{t}:{f.hex()}" for t, f in frames))
        r = subprocess.run([str(dock.live()), "--bin", str(APP), "--shot", str(ms), "--usb"], cwd=wd, env=env,
                           capture_output=True, text=True)
    got = bytes.fromhex("".join(ln.split("usb in:")[1] for ln in r.stdout.splitlines() if "usb in:" in ln))
    return [x for x in (hbdeploy.parse(m) for m in re.findall(rb"\xf0\x7dHB[^\xf7]*\xf7", got)) if x]


def rms_left(s, t0, t1):
    """the left channel's RMS between t0 and t1 s (as peak's times)"""
    x = s[int(t0 * 44100) * 2:int(t1 * 44100) * 2:2]
    return (sum(v * v for v in x) / max(1, len(x))) ** 0.5


def sym(name):
    m = re.search(rf"^([0-9a-f]{{8}}) .*\s{name}$", (SRC / "build" / "fm1-dock.syms").read_text(), re.M)
    return int(m.group(1), 16) - 0x01C00000


def midi_out(case):
    """what the FM-1 sent over USB-MIDI in the run (usb.c's ring, the last 64): [(status, data 1, data 2)]"""
    ram = (OUT / case / "ram.bin").read_bytes()
    (w,) = struct.unpack_from("<I", ram, sym("mo_w"))
    q = struct.unpack_from("<64I", ram, sym("midi_out_q"))
    return [(q[i % 64] >> 8 & 0xFF, q[i % 64] >> 16 & 0xFF, q[i % 64] >> 24) for i in range(max(0, w - 64), w)]


def run(case, keys, ms, binp=APP, extra=()):
    r = subprocess.run([sys.executable, str(SRC / "tools" / "dock.py"), "emu", "--bin", str(binp), "--ms", str(ms),
                        "--keys", keys, "--out", str(OUT / case), *extra], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"{case}: {r.stderr or r.stdout}")
    w = wave.open(str(OUT / case / "audio.wav"))
    d = w.readframes(w.getnframes())
    s = struct.unpack(f"<{len(d) // 2}h", d)
    log = (OUT / case / "log.txt").read_text()
    return s, log


def peak(s, t0, t1):
    """the peak between t0 and t1 s of the WAV (it starts when the audio does, ~0.2 s after boot)"""
    return max((abs(x) for x in s[int(t0 * 44100) * 2:int(t1 * 44100) * 2]), default=0)


def check(case, ok, why):
    print(f"  {'ok  ' if ok else 'FAIL'} {case}: {why}")
    if not ok:
        failures.append(case)


def onset_frames(s):
    first = next((i for i, x in enumerate(s) if x), len(s))
    return s[first - first % 2:]


LINE = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*?(\t.*| +<unknown instruction>)$")
NEVER_RUN = {"wfe", "sevl", "sevg", "trigger",   # (multicore events in library code; the host's debug trigger;
             "syscall"}                         # no OS: in a module, text such as GATOR's strings in .text)


def check_decode(emu, lines):
    """fm1-emu --check-decode on hex lines -> [(length, name, status)]"""
    with tempfile.NamedTemporaryFile("w", suffix=".txt") as f:
        f.write("\n".join(lines) + "\n")
        f.flush()
        out = subprocess.run([str(emu), "--check-decode", f.name], capture_output=True, text=True).stdout.splitlines()
    if len(out) != len(lines):
        return None
    return [(int(o.split("|")[1]), o.split("|")[2], o.split("|")[-1]) for o in out]


def decode_check():
    """every instruction of the host and the modules through the emulator's own decoder (fm1-emu
    --check-decode, next to fm1-live): what it cannot run fails here, not mid-test. The code is walked by the
    emulator's own instruction lengths: the disassembler does not know the FPU's instructions (`<unknown
    instruction>`, two bytes at a time), so its boundaries after one are wrong and would show made-up instructions"""
    emu = Path(os.environ.get("FM1_LIVE", Path.home() / "work/fm1-emu/target/release/fm1-live")).with_name("fm1-emu")
    for dis in [SRC / "build" / "fm1-dock.dis", *sorted((SRC / "build" / "modules").glob("*/module.dis"))]:
        name = dis.parent.name if dis.parent.name != "build" else "host"
        mem, asm, starts = {}, {}, []           # the bytes by address, the disassembler's text, its row starts
        for m in map(LINE.match, dis.read_text().splitlines()):
            if not m:
                continue
            at = int(m.group(1), 16)
            for i, b in enumerate(m.group(2).split()):
                mem[at + i] = int(b, 16)
            asm[at] = m.group(3).strip()
            starts.append(at)
        lost = {u + d for u in asm if asm[u].startswith("<") for d in range(0, 24, 2)}   # (where the disassembler lost step)
        even = sorted(a for a in set(starts) | lost if a in mem)
        res = check_decode(emu, ["".join(f"{mem.get(a + i, 0):02x}" for i in range(8)) for a in even])
        if res is None:
            check(f"decode {name}", False, f"{emu} has no --check-decode (an fm1-emu older than 2026-10-19?)")
            continue
        seen = dict(zip(even, res))             # address -> (length, name, status)
        rows = sorted(set(starts))
        insns, a = [], rows[0]
        for s in rows:                          # each run of the disassembler's rows, by the emulator's lengths
            if s < a:
                continue
            a = s
            while a in seen and seen[a][0]:     # (undecoded: data, on at the disassembler's next row)
                insns.append(a)
                a += seen[a][0]
            a += 2
        text = lambda a: asm[a] if a in asm and not asm[a].startswith("<") else seen[a][1]
        bad = sorted({text(a) for a in insns if seen[a][2] != "ok" and text(a).split()[0] not in NEVER_RUN})
        check(f"decode {name}", not bad, f"{len(insns)} instructions, all known to the emulator" if not bad else
              f"unknown to the emulator: {', '.join(bad[:6])}")


def p5_frame(name, prog, cutoff):
    """a Prophet-5/10 native single-program dump (Melodee's prophet_patch.c layout: 133 raw bytes, 7 to a group
    behind their top bits), a plain saw: what a player's own bank holds, made here (no Sequential data)"""
    raw = bytearray(133)
    for i, v in {0: 24, 1: 24, 3: 1, 8: 64, 9: 64, 12: 1, 14: 127, 17: cutoff, 20: 1, 45: 60, 46: 60, 47: 127,
                 48: 127, 49: 35, 50: 35, 51: 1, 53: 5}.items():
        raw[i] = v
    raw[65:85] = name.encode().ljust(20)
    f = bytearray([0xF0, 0x01, 0x32, 0x02, prog // 40, prog % 40])
    for g in range(0, 133, 7):
        part = raw[g:g + 7]
        f.append(sum((b >> 7) << j for j, b in enumerate(part)))
        f += bytes(b & 0x7F for b in part)
    return bytes(f + b"\xf7")


def upward(s, t0, t1):
    """the left channel's upward zero crossings per second between t0 and t1 s: a saw's frequency"""
    x = s[int(t0 * 44100) * 2:int(t1 * 44100) * 2:2]
    return sum(1 for a, b in zip(x, x[1:]) if a < 0 <= b) / (t1 - t0)


def brightness(s, t0, t1):
    """the RMS of the left channel's first difference over its RMS: how much of it is high"""
    x = s[int(t0 * 44100) * 2:int(t1 * 44100) * 2:2]
    d = [b - a for a, b in zip(x, x[1:])]
    return (sum(v * v for v in d) / max(1, len(d))) ** 0.5 / max(1.0, (sum(v * v for v in x) / max(1, len(x))) ** 0.5)


def prophet_cases():
    """PROPHET: Melodee's Prophet-5 engine (modules/prophet)"""
    import hbdeploy
    chord = "".join(f",1800:{k},3200:{k}^" for k in (14, 16, 18, 19, 21, 23, 25, 26))
    # STRINGS (patch 3: pulse and saw, the wheel's PWM, the SSI filter, the heaviest of its own) as an eight-note
    # chord, four voices on the second core; then a unit whose core 1 misreads: one core, the Prophet's five voices
    s, log = run("prophet", pick("PROPHET") + ",1300:e6:3" + chord, 4000)
    worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
    check("prophet", "prophet: Melodee" in log and "cpu1: 2 cores" in log and peak(s, 0.0, 1.5) == 0 and
          peak(s, 1.7, 3.0) > 1000 and " 0 overruns" in log and worst < 60,
          f"PROPHET plays STRINGS, eight voices, four on the second core; core 0's worst {worst} % of a block")
    s, log = run("prophet_flaky", pick("PROPHET") + ",1300:e6:3" + chord, 4000,
                 extra=("--flaky-core1", "1e-6", "--seed", "2"))
    worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
    check("prophet_flaky", "cpu1: misread RAM, 1 core" in log and peak(s, 1.7, 3.0) > 1000 and " 0 overruns" in log
          and worst < 80, f"a faulty second core: PROPHET plays five voices on core 0; worst {worst} %")

    # MIDI in: INIT's saw on A4 (440 Hz), bent up to its range (one semitone); program change 2 (BRASS: velocity to
    # the amplifier and the filter), A4 soft then hard; CC 74 (cutoff) closes INIT's filter
    m = (pick("PROPHET") + ",1400:m904564,2000:me07f7f,2600:m804500,2650:me00040,2700:mc002,2900:m904514,"
         "3500:m804500,3800:m90457f,4400:m804500,4700:mc000,4800:m903964,5400:mb04a10,6000:m803900")
    s, log = run("prophet_midi", m, 6200)
    t = onset_frames(s)                           # (times from the first note, 1.4 s)
    f1, f2 = upward(t, 0.15, 0.55), upward(t, 0.75, 1.15)
    soft, hard = rms_left(t, 1.65, 2.05), rms_left(t, 2.55, 2.95)
    open_, shut = brightness(t, 3.55, 3.95), brightness(t, 4.25, 4.55)
    check("prophet_midi", abs(f1 - 440) < 8 and abs(f2 - 466.2) < 8 and hard > 3 * soft > 0 and open_ > 2 * shut > 0,
          f"PROPHET over MIDI: A4 at {f1:.0f} Hz, bent {f2:.0f} Hz; BRASS by program change, RMS {soft:.0f} soft, "
          f"{hard:.0f} hard; CC 74 closes the filter (brightness {open_:.2f} -> {shut:.2f})")

    # a player's own bank (tools/dock.py send FILE.syx: MODULE frames, here into the DIN jack): three programs,
    # kept in the module's flash banks; the next power-on lists them after the module's own nine
    syx = b"".join(p5_frame(f"HARBOR TEST {i}", i, 100) for i in range(3))
    steps = [b"B" + struct.pack("<II", len(syx), 0)]
    steps += [b"D" + struct.pack("<I", a) + syx[a:a + 256] for a in range(0, len(syx), 256)] + [b"E"]
    k, at = [pick("PROPHET")], 1500
    for i, st in enumerate(steps):
        f = hbdeploy.frame(hbdeploy.MODULE, i, st)
        k.append(f"{at}:m{f.hex()}")
        at += int(len(f) * hbdeploy.BYTE_MS) + 30
    flash = OUT / "prophet_flash.bin"
    flash.unlink(missing_ok=True)
    s, log = run("prophet_bank", ",".join(k) + f",{at + 400}:21,{at + 900}:21^", at + 1500, extra=("--flash", str(flash)))
    s2, log2 = run("prophet_bank2", pick("PROPHET") + ",1800:21,2300:21^", 2600, extra=("--flash", str(flash)))
    check("prophet_bank", "prophet: programs in 00000003" in log and peak(s, at / 1000 - 0.1, at / 1000 + 0.6) > 1000
          and "programs 0000000c" in log2 and peak(s2, 1.7, 2.2) > 1000,
          "a bank sent to PROPHET (three programs) plays and is there after a power-on (12 programs)")


def fmini_cases():
    """FMINI-1: FMini-1's FMINI engine and its 200 factory patches (modules/fmini)"""
    # MINI LEAD (F001, LEGATO): a key sounds, silent before, and stops
    s, log = run("fmini", pick("FMINI-1") + ",1800:21,2800:21^", 4200)
    worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
    check("fmini", "fmini: FMini-1 2.2.0, patches 000000c8" in log and peak(s, 0.0, 1.5) == 0 and
          peak(s, 1.9, 2.7) > 1000 and peak(s, 3.9, 4.1) < peak(s, 1.9, 2.7) // 20 and " 0 overruns" in log,
          f"FMINI-1 plays MINI LEAD on a key and stops; worst {worst} % of a block")

    # SINE SOLO (F005: a triangle at 8', LEGATO, GLIDE on) by program change: E4 held, then A3 legato (FMini-1's
    # LOW note priority: the lower key wins): the pitch glides from 330 Hz down to 220 Hz on one contour (no gap); A3
    # let go: back to E4, still held
    m = (pick("FMINI-1") + ",1300:mc004,1500:m904064,2400:m903964,3600:m803900,4600:m804000")
    s, log = run("fmini_glide", m, 5000)
    t = onset_frames(s)                           # (times from the first note, 1.5 s)
    f1, fmid, f2, f3 = upward(t, 0.4, 0.85), upward(t, 0.92, 1.0), upward(t, 1.6, 2.05), upward(t, 2.6, 3.0)
    gap = min(rms_left(t, 0.85 + 0.01 * i, 0.86 + 0.01 * i) for i in range(20))
    check("fmini_glide", abs(f1 - 329.6) < 8 and f2 + 10 < fmid < f1 - 10 and abs(f2 - 220) < 6 and abs(f3 - 329.6) < 8
          and gap > 0.2 * rms_left(t, 0.4, 0.85), f"LEGATO and GLIDE: E4 {f1:.0f} Hz, gliding ({fmid:.0f}) to A3 "
          f"{f2:.0f} Hz on one contour (the lowest 10 ms {gap:.0f}), back to E4 {f3:.0f} Hz")

    # PAD SWELL (F004, POLY): an eight-note chord, half of its voices on the second core
    chord = "".join(f",1800:{k},3400:{k}^" for k in (14, 16, 18, 19, 21, 23, 25, 26))
    s, log = run("fmini_poly", pick("FMINI-1") + ",1300:e6:3" + chord, 4000)
    worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
    check("fmini_poly", "cpu1: 2 cores" in log and peak(s, 2.0, 3.3) > 1000 and " 0 overruns" in log and worst < 70,
          f"FMINI-1 plays PAD SWELL as an eight-note chord on two cores; core 0's worst {worst} % of a block")

    # four patches on one note sound different: MOOG BASS, UFO, MOOG FLUTE, ACID LEAD (program changes 1, 9, 20, 15);
    # then CC 74 (CUTOFF) down closes MOOG BASS's ladder
    m = pick("FMINI-1")
    for i, p in enumerate((1, 9, 20, 15)):
        m += f",{1300 + 1000 * i}:mc0{p:02x},{1500 + 1000 * i}:m903064,{2100 + 1000 * i}:m803000"
    m += ",5300:mc001,5500:m903064,5900:mb04a08,6300:m803000"
    s, log = run("fmini_presets", m, 6600)
    t = onset_frames(s)
    b = [brightness(t, 0.15 + i, 0.5 + i) for i in range(4)] + [brightness(t, 4.15, 4.35), brightness(t, 4.55, 4.75)]
    r = [rms_left(t, 0.15 + i, 0.5 + i) for i in range(4)]
    differ = all(abs(b[i] - b[j]) > 0.02 or abs(r[i] - r[j]) > 0.2 * max(r[i], r[j]) for i in range(4) for j in range(i))
    check("fmini_presets", all(x > 300 for x in r) and differ and b[5] < 0.6 * b[4],
          f"four factory patches, four sounds (brightness {', '.join(f'{x:.2f}' for x in b[:4])}); CC 74 closes the "
          f"ladder ({b[4]:.2f} -> {b[5]:.2f})")

    # the arpeggiator: SINE SOLO (its TEMPO 120), ARP on (the ARP button: UP, 1/16, one octave), C4 and G4 held: the two
    # notes in turn, a step each 125 ms; with HOLD (CC 60) the arpeggio goes on after the keys are let go
    def steps(t, t0, t1):
        f = [upward(t, t0 + 0.125 * i + 0.02, t0 + 0.125 * i + 0.06) for i in range(int((t1 - t0) / 0.125))]
        return sum(1 for x in f if abs(x - 261.6) < 40), sum(1 for x in f if abs(x - 392) < 50), f
    m = pick("FMINI-1") + ",1300:mc004,1500:10,1550:10^,1800:m903c64,1800:m904364,3300:m803c00,3300:m804300"
    s, log = run("fmini_arp", m, 4200)
    lo, hi, f = steps(s, 1.9 - 0.2, 3.1 - 0.2)    # (the WAV starts ~0.2 s after boot)
    after = rms_left(s, 3.6 - 0.2, 4.0 - 0.2)
    check("fmini_arp", lo >= 4 and hi >= 4 and after < 0.1 * rms_left(s, 2.0, 2.9),
          f"ARP on SINE SOLO: C4 and G4 in turn at 1/16 (steps: {lo} C4, {hi} G4), silent once the keys are let go")
    s, log = run("fmini_hold", m.replace(",1800:m903c64", ",1700:mb03c7f,1800:m903c64"), 4600)
    lo2, hi2, _ = steps(s, 3.5 - 0.2, 4.4 - 0.2)
    check("fmini_hold", lo2 >= 2 and hi2 >= 2, f"HOLD: the arpeggio goes on after the keys are let go ({lo2} C4, {hi2} G4 steps)")

    # PLUCK ARP (F003): its own arpeggio on recall (UP-DOWN, 1/16, three octaves): one key held, the steps climb and fall
    # over three octaves
    s, log = run("fmini_arppatch", pick("FMINI-1") + ",1300:mc002,1800:m903064,3300:m803000", 3600)
    f = [upward(s, 1.6 + 0.125 * i + 0.02, 1.6 + 0.125 * i + 0.09) for i in range(10)]
    check("fmini_arppatch", min(f) > 0 and max(f) > 2.5 * min(f),
          f"PLUCK ARP arpeggiates by itself over its octaves (steps from {min(f):.0f} to {max(f):.0f} crossings a second)")

    # the effects: SINE SOLO (its sends: chorus 51, delay 32, reverb 32; the delay ANALOG, 374 ms), a short note: its tail
    # 0.4 .. 1.2 s after the note, with the effects and with every send at 0 (CC 91, 94, 93)
    k = pick("FMINI-1") + ",1300:mc004,1800:m904564,1900:m804500"
    s, log = run("fmini_fx", k, 3400)
    s2, log2 = run("fmini_dry", k.replace(",1800:", ",1500:mb05b00,1520:mb05e00,1540:mb05d00,1800:"), 3400)
    wet, dry = rms_left(s, 2.1, 2.9), rms_left(s2, 2.1, 2.9)
    check("fmini_fx", wet > 3 * dry and wet > 20 and " 0 overruns" in log,
          f"FMini-1's effects: a short note's tail {wet:.0f} with them, {dry:.0f} with every send at 0")

    # a user patch: MOOG BASS with its cutoff down (CC 74), SAVE twice into U001; after a power-off FMINI-1 lists
    # 201 patches and starts on it
    flash = OUT / "fmini_flash.bin"
    flash.unlink(missing_ok=True)
    k = pick("FMINI-1") + ",1300:mc001,1500:mb04a10,1700:9,1750:9^,1900:9,1950:9^"
    s, log = run("fmini_save", k, 5000, extra=("--flash", str(flash)))
    s2, log2 = run("fmini_saved", "1800:21,2300:21^", 2600, extra=("--flash", str(flash)))
    ram = (OUT / "fmini_saved" / "ram.bin").read_bytes()
    check("fmini_save", "patches 000000c9" in log2 and peak(s2, 1.9, 2.3) > 1000 and b"MOOG BASS" in ram,
          "a sound saved into U001 is there after a power-off (201 patches) and plays")


def dx7_voice(name, coarse):
    """a DX7 single-voice dump (VCED, 155 bytes: OP6 first): OP1 alone, a sine at coarse x the note, made here"""
    v = []
    for op in range(6):                         # R1-4, L1-4, BP, LD, RD, LC, RC, RS, AMS, KVS, OL, MODE, FC, FF, DET
        v += [99, 99, 99, 99, 99, 99, 99, 0, 39, 0, 0, 0, 0, 0, 0, 0, 99 if op == 5 else 0, 0, coarse, 0, 7]
    v += [99] * 4 + [50] * 4 + [0, 0, 1, 35, 0, 0, 0, 1, 0, 3, 24] + list(name.encode().ljust(10))
    return bytes([0xF0, 0x43, 0x00, 0x00, 0x01, 0x1B] + v + [-sum(v) & 0x7F, 0xF7])


def atek_cases():
    """ATEK303: Animatek's acid machine (modules/atek303): its effects on the second core, on core 0 without it"""
    s, log = run("atek303", pick("ATEK303") + ",2000:12,2050:12^", 5000)
    worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
    check("atek303", "atek303: ATEK303 1.2" in log and peak(s, 0.0, 1.8) == 0 and peak(s, 2.5, 5.0) > 1000 and
          " 0 overruns" in log and worst < 85, f"ATEK303: PLAY starts its pattern; worst {worst} % of a block")
    s, log = run("atek303_key", pick("ATEK303") + ",2000:21,3500:21^", 4000)
    check("atek303_key", peak(s, 0.0, 1.8) == 0 and peak(s, 2.1, 3.4) > 1000 and " 0 overruns" in log,
          "ATEK303: a key plays its voice")
    s, log = run("atek303_flaky", pick("ATEK303") + ",2000:12,2050:12^", 5000,
                 extra=("--flaky-core1", "1e-6", "--seed", "2"))
    check("atek303_flaky", "cpu1: misread RAM, 1 core" in log and peak(s, 2.5, 5.0) > 1000 and " 0 overruns" in log,
          "ATEK303 with a faulty second core: its effects on core 0, still in time")


def fm6_cases():
    """FM6: Melodee's six-operator FM engine and its own voices (modules/fm6)"""
    import hbdeploy
    # TINE EP (F1, the first voice): a key sounds, silent before, and dies away
    s, log = run("fm6", pick("FM6") + ",1800:21,2600:21^", 5000)
    worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
    check("fm6", "fm6: Melodee 165d177, voices 00000019" in log and peak(s, 0.0, 1.5) == 0 and
          peak(s, 1.7, 2.5) > 1000 and peak(s, 4.6, 5.0) < peak(s, 1.7, 2.5) // 20 and " 0 overruns" in log,
          f"FM6 plays TINE EP on a key and it dies away; worst {worst} % of a block")

    # STRINGS (F16: six operators, held) as a 16-note chord, half of its voices on the second core
    chord = "".join(f",1800:{k},3600:{k}^" for k in (14, 16, 18, 19, 21, 23, 25, 26, 28, 30, 31, 33, 35, 37, 38, 40))
    s, log = run("fm6_chord", pick("FM6") + ",1300:e6:15" + chord, 4200)
    worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
    check("fm6_chord", "cpu1: 2 cores" in log and peak(s, 2.0, 3.4) > 1000 and " 0 overruns" in log and worst < 80,
          f"FM6 plays STRINGS as a 16-note chord on two cores; core 0's worst {worst} % of a block")
    s, log = run("fm6_flaky", pick("FM6") + ",1300:e6:15" + chord, 4200, extra=("--flaky-core1", "1e-6", "--seed", "2"))
    worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
    check("fm6_flaky", "cpu1: misread RAM, 1 core" in log and peak(s, 2.0, 3.4) > 1000 and " 0 overruns" in log
          and worst < 85, f"a faulty second core: FM6 plays ten voices on core 0; worst {worst} %")

    # four voices on one note sound different: TINE EP, GLASS BELL, BRASS SECT, SOLID BASS (program changes 1, 2, 10, 11)
    m = pick("FM6")
    for i, p in enumerate((1, 2, 10, 11)):
        m += f",{1300 + 1000 * i}:mc0{p:02x},{1500 + 1000 * i}:m903c64,{2100 + 1000 * i}:m803c00"
    s, log = run("fm6_presets", m, 5400)
    t = onset_frames(s)
    b = [brightness(t, 0.05 + i, 0.4 + i) for i in range(4)]
    r = [rms_left(t, 0.05 + i, 0.4 + i) for i in range(4)]
    differ = all(abs(b[i] - b[j]) > 0.02 or abs(r[i] - r[j]) > 0.2 * max(r[i], r[j]) for i in range(4) for j in range(i))
    check("fm6_presets", all(x > 300 for x in r) and differ,
          f"four of Melodee's voices, four sounds (brightness {', '.join(f'{x:.2f}' for x in b)}, "
          f"RMS {', '.join(f'{x:.0f}' for x in r)})")

    # a DX7 voice dump (tools/dock.py send FILE.syx: MODULE frames, here into the DIN jack): OP1 alone at x2, played at
    # once (A4 over MIDI: 880 Hz), kept in the module's flash banks; the next power-on lists it after Melodee's 25
    syx = dx7_voice("HARBOR X2", 2)
    steps = [b"B" + struct.pack("<II", len(syx), 0)]
    steps += [b"D" + struct.pack("<I", a) + syx[a:a + 256] for a in range(0, len(syx), 256)] + [b"E"]
    k, at = [pick("FM6")], 1500
    for i, st in enumerate(steps):
        f = hbdeploy.frame(hbdeploy.MODULE, i, st)
        k.append(f"{at}:m{f.hex()}")
        at += int(len(f) * hbdeploy.BYTE_MS) + 30
    flash = OUT / "fm6_flash.bin"
    flash.unlink(missing_ok=True)
    s, log = run("fm6_sysex", ",".join(k) + f",{at + 400}:m904564,{at + 1400}:m804500", at + 1600,
                 extra=("--flash", str(flash)))
    t = onset_frames(s)
    f1 = upward(t, 0.2, 0.9)
    s2, log2 = run("fm6_sysex2", pick("FM6") + ",1800:21,2300:21^", 2600, extra=("--flash", str(flash)))
    check("fm6_sysex", "fm6: voices in 00000001" in log and abs(f1 - 880) < 10 and "voices 0000001a" in log2
          and peak(s2, 1.7, 2.2) > 1000,
          f"a DX7 voice dump sent to FM6 plays at once (A4 at {f1:.0f} Hz: its OP1 at x2) and is there after a power-on")


def niagara_cases():
    """NIAGARA: Evan Yerburgh's Niagara Drift (modules/niagara), two drones"""
    k = pick("NIAGARA") + ",2000:12,2050:12^"   # PLAY: the drones fade in (its start animation ends on the press)
    s, log = run("niagara", k, 4500)
    worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
    check("niagara", "niagara: Niagara Drift" in log and peak(s, 0.0, 1.7) == 0 and peak(s, 3.0, 4.2) > 1000 and
          " 0 overruns" in log, f"NIAGARA plays its drones on PLAY, silent before; worst {worst} % of a block")
    # the twelve Presets (the scene keys, the lower white keys: F3 .. D5 but the black ones), one after the other
    scenes = "".join(f",{2600 + 700 * i}:{14 + w},{2700 + 700 * i}:{14 + w}^"
                     for i, w in enumerate((0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19)))
    s, log = run("niagara_presets", k + scenes, 11400)
    worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
    check("niagara_presets", peak(s, 10.0, 11.0) > 1000 and " 0 overruns" in log and worst < 70,
          f"NIAGARA through its twelve Presets; worst {worst} % of a block")
    # KNOB2 (drone I's cutoff, the Play page), a note on MIDI channel 2 (drone II's pitch), a scene key's pressure:
    # each changes the sound from where it happens (the WAV starts ~0.2 s after power-on), nothing before
    a, _ = run("niagara_still", k + ",4000:23@100,5000:23^", 5500)
    for case, more, t in (("niagara_knob", ",3500:e3:6", 3.5), ("niagara_midi", ",3500:m913c64", 3.5),
                          ("niagara_pressure", ",4400:23~127", 4.4)):
        b, _ = run(case, k + more + ",4000:23@100,5000:23^", 5500)
        n = int((t - 0.3) * 44100) * 2
        first = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), len(a)) / 88200
        check(case, a[:n] == b[:n] and t - 0.3 < first < t + 0.3, f"NIAGARA: {case[8:]} changes the drones from {first:.2f} s")
    # AUTOSAVE (its own: ~4 s after the last change, the drones stopped): KNOB2 turned past the start animation, its
    # project in the module's bank 1; at the next power-on (NIAGARA again, the last module) it loads back
    flash = OUT / "niagara_flash.bin"
    flash.unlink(missing_ok=True)
    fw = ("--fwsc", str(SRC / "build" / "fm1-dock.fwsc"), "--flash", str(flash))
    run("niagara_fresh", pick("NIAGARA"), 4500, extra=fw)   # (the host keeps NIAGARA as the last module)
    fresh = (OUT / "niagara_fresh" / "ram.bin").read_bytes()
    run("niagara_save", "5500:e3:-20", 12000, extra=fw)
    _, log = run("niagara_saved", "", 3000, extra=fw)
    ram = (OUT / "niagara_saved" / "ram.bin").read_bytes()
    at = fresh.find(b"DRN1")
    check("niagara_saved", "niagara: Niagara Drift" in log and at >= 0 and ram.find(b"DRN1") == at and
          ram[at:at + 512] != fresh[at:at + 512], "NIAGARA's sound is there after a power-off (its project autosaved "
          "to its bank, loaded back)")


def keys_cases():
    """KEYS: a clavinet, an electric piano and a piano from physical models (modules/keys)"""
    # each family on a key (program changes 0, 4, 7: CLAV BRIGHT, EP SOFT, GRAND), silent before; the key let go
    # lowers the damper: quiet within a second
    for case, prog in (("keys_clav", 0), ("keys_ep", 4), ("keys_piano", 7)):
        s, log = run(case, pick("KEYS") + f",1300:mc0{prog:02x},1800:21,2600:21^", 3800)
        worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
        check(case, "keys: presets 0000000a" in log and peak(s, 0.0, 1.5) == 0 and peak(s, 1.7, 2.3) > 1000 and
              peak(s, 3.4, 3.6) < peak(s, 1.7, 2.3) // 20 and " 0 overruns" in log,
              f"KEYS plays {case[5:].upper()} on a key and damps it; worst {worst} % of a block")

    # the velocity: a hard key louder (and so brighter) than a soft one, in each family
    for fam, prog in (("clav", 0), ("ep", 4), ("piano", 7)):
        k = pick("KEYS") + f",1300:mc0{prog:02x}"
        a, _ = run(f"keys_{fam}_soft", k + ",1800:23@25,2600:23^", 2800)
        b, _ = run(f"keys_{fam}_hard", k + ",1800:23@127,2600:23^", 2800)
        check(f"keys_{fam}_vel", peak(b, 1.5, 2.3) > 2 * peak(a, 1.5, 2.3) > 0,
              f"KEYS {fam.upper()} at the key's velocity (peak {peak(a, 1.5, 2.3)} soft, {peak(b, 1.5, 2.3)} hard)")

    # GRAND (the heaviest: two strings a voice) as a 16-note chord, half of its voices on the second core; then the
    # same on a faulty second core: all 16 voices on core 0
    chord = "".join(f",1800:{k},3600:{k}^" for k in (14, 16, 18, 19, 21, 23, 25, 26, 28, 30, 31, 33, 35, 37, 38, 40))
    s, log = run("keys_chord", pick("KEYS") + ",1300:mc007" + chord, 4200)
    worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
    check("keys_chord", "cpu1: 2 cores" in log and peak(s, 2.0, 3.4) > 1000 and " 0 overruns" in log and worst < 80,
          f"KEYS plays GRAND as a 16-note chord on two cores; core 0's worst {worst} % of a block")
    s, log = run("keys_flaky", pick("KEYS") + ",1300:mc007" + chord, 4200, extra=("--flaky-core1", "1e-6", "--seed", "2"))
    worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
    check("keys_flaky", "cpu1: misread RAM, 1 core" in log and peak(s, 2.0, 3.4) > 1000 and " 0 overruns" in log
          and worst < 85, f"a faulty second core: KEYS plays its 16 voices on core 0; worst {worst} %")

    # one family after another on the same voices (what one leaves in a voice is not the next one's): EP BELL's chord,
    # then CLAV WAH's and GRAND's on the same keys while the last still rings
    keys = (14, 18, 21, 25, 28)
    m = pick("KEYS")
    for i, p in enumerate((6, 3, 7)):
        m += f",{1300 + 700 * i}:mc0{p:02x}" + "".join(f",{1400 + 700 * i}:{k},{1900 + 700 * i}:{k}^" for k in keys)
    s, log = run("keys_switch", m, 3800)
    check("keys_switch", all(peak(s, 1.3 + 0.7 * i, 1.7 + 0.7 * i) > 1000 for i in range(3)) and " 0 overruns" in log
          and "strikes: none" in log, "KEYS: E.PIANO, CLAV and PIANO in turn on the same voices")

    # the families on one note sound different: CLAV BRIGHT, EP SOFT, HONKY (program changes 0, 4, 9)
    m = pick("KEYS")
    for i, p in enumerate((0, 4, 9)):
        m += f",{1300 + 1000 * i}:mc0{p:02x},{1500 + 1000 * i}:m903c64,{2100 + 1000 * i}:m803c00"
    s, log = run("keys_presets", m, 4400)
    t = onset_frames(s)
    b = [brightness(t, 0.02 + i, 0.4 + i) for i in range(3)]
    r = [rms_left(t, 0.02 + i, 0.4 + i) for i in range(3)]
    differ = all(abs(b[i] - b[j]) > 0.02 or abs(r[i] - r[j]) > 0.2 * max(r[i], r[j]) for i in range(3) for j in range(i))
    check("keys_presets", all(x > 300 for x in r) and differ,
          f"three families, three sounds (brightness {', '.join(f'{x:.2f}' for x in b)}, RMS {', '.join(f'{x:.0f}' for x in r)})")


def main():
    names = [n for n, _, _ in slots()]
    print(f"slots: {', '.join(names)}")
    decode_check()

    s, log = run("boot", "1000:14,1600:14^", 2200)
    check("boot", "started 00000000" in log and peak(s, 0.9, 1.3) > 1000 and peak(s, 1.8, 2.0) == 0,
          "the built-in sine starts and plays a key")
    s, log = run("boot_safe", "0:1,400:1^,1000:14,1600:14^", 2200)
    check("boot_safe", "safe boot" in log and "started 00000000" in log and peak(s, 0.9, 1.3) > 1000 and peak(s, 1.8, 2.0) == 0,
          "OCT+ held at power-on: the built-in sine starts and plays a key")

    # OPTIONS, ABOUT: its page, the version on its first line in the accent colour (8 px a character, from x 4);
    # HOME: OPTIONS again, ABOUT the line picked
    at = f"700:8,750:8^,900:e6:{1 + len(names) + O_ABOUT},1100:3,1150:3^"
    _, log = run("about", at, 1700)
    ver = (re.findall(r"fm1-dock (\S+) boot", log) or ["?"])[-1]
    x0, x1 = acc_span("about", 28, 44)
    _, _ = run("about_back", at + ",1500:8,1550:8^", 2000)
    y = 50 + O_ABOUT * 16                   # (dock.c OPT_Y)
    b0, b1 = acc_span("about_back", y, y + 18)
    n = len(f"fm1-dock {ver}") + (10 if "." in ver else 0)   # (a release's: " (abc1234)" after it)
    check("about", re.fullmatch(r"\d\.\d(\.\d)?|[0-9a-f]{7}\+?|unknown", ver) is not None and x0 >= 2 and
          abs(x1 - x0 - 8 * n) <= 8 and acc_span("about_back", 28, 44) == (0, 0) and b1 - b0 >= 30,
          f"ABOUT: fm1-dock {ver} (the boot log) on its page, {x1 - x0} px; HOME: OPTIONS, ABOUT picked ({b1 - b0} px)")

    # INFO (0x03) over USB, as tools/dock.py info and the page ask it: ABOUT's facts in a record; an unknown command
    # (0x04) answered bad message, as an older fm1-dock answers INFO
    import hbdeploy
    rep = usb_ask([(1500, hbdeploy.frame(hbdeploy.INFO, 5)), (1600, hbdeploy.frame(0x04, 6))], 2000)
    info = [d for c, q, d in rep if c == hbdeploy.INFO and q == 5]
    i = hbdeploy.info(type("P", (), {"ask": lambda self, *_: info[0]})()) if info else None
    check("info", i is not None and i["version"] == ver and i["model"] == "FM-1" and i["board"] == "FM-1" and
          i["flash_id"] in (0x856014, 0xB36014, 0xEB6014) and i["flash_size"] == 1 << 20 and i["soc"] == "AC791N" and
          i["mod_ram"] == 0x44000 and 0 < i["largest"] <= i["free"] and i["cores"] in (1, 2) and
          any(c == 0x04 and q == 6 and d[:1] == b"\x0a" for c, q, d in rep),
          f"INFO: fm1-dock {i and i['version']} ({i and i['build']}) on {i and i['model']}, {i and i['soc']}, flash "
          f"{i and i['flash_id']:06X}, {i and i['cores']}CPU, add-ons {i and i['free'] // 1024} KiB free; 0x04: bad message"
          if i else f"INFO: no record ({rep})")

    if "SINE" in names:
        # (a top-row key: a sensed one's lift is seen at the key scan's phase, which the picker shifts)
        a, _ = run("sine_builtin", "1500:34,1900:34^", 2600)
        b, log = run("sine_slot", pick("SINE") + ",1500:34,1900:34^", 2600)
        a, b = onset_frames(a), onset_frames(b)
        n = min(len(a), len(b))
        check("sine_slot", n > 40000 and a[:n] == b[:n] and "started 0000000" in log,
              f"the sine from its slot is bit-identical to the built-in one ({n // 2} frames)")

    if "SID" in names:
        s, log = run("sid", pick("SID") + ",1800:14,1800:18,1800:21,3000:14^,3000:18^,3000:21^", 3600)
        st = log.split("render: last ")[1]
        worst = float(st.split("worst ")[1].split("(")[1].split(" %")[0])
        check("sid", "sid: FM-1 SID's synth" in log and peak(s, 1.7, 2.6) > 1000 and peak(s, 0.0, 1.5) == 0 and
              " 0 overruns" in log and worst < 50, f"SID (FM-1 SID, reSID) plays a chord, no click before it; worst {worst} % of a block")

        if "CS-1" in names:                     # HOME held 1.5 s in a panel module: the list; then CS-1
            idx = 1 + names.index("CS-1") - (1 + names.index("SID"))
            s, log = run("home_hold", pick("SID") + f",1600:8,3400:8^,3600:e6:{idx},3800:8,3850:8^,4300:14,4700:14^", 5000)
            starts = [ln.split()[-1] for ln in log.splitlines() if ln.split()[1:2] == ["started"]]
            check("home_hold", log.count("started") == 3 and "cs1: CS-1" in log and peak(s, 4.1, 4.5) > 500,
                  "HOME held in SID opens the module list; CS-1 loads from it and plays")

        img = bytearray(APP.read_bytes())
        at = dict((n, a) for n, a, _ in slots())["SID"]
        img[at - 0x02000000 + 0x1000] ^= 0x40          # a bit in its code
        bad = SRC / "build" / "fm1-dock_badcrc_xip.bin"
        bad.write_bytes(img)
        s, log = run("bad_crc", pick("SID") + ",1600:14,2000:14^", 2500, bad)
        check("bad_crc", "slot refused" in log and "SID" not in log.split("started")[-1] and peak(s, 1.3, 1.8) > 1000,
              "a module with a bad CRC is refused; the sine plays on")

    if "CS-1" in names:
        keys = pick("CS-1") + "".join(f",1800:{k},3200:{k}^" for k in (14, 16, 18, 19, 21, 23, 25, 26))
        s, log = run("cs1", keys, 4000)
        worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
        check("cs1", "cs1: CS-1" in log and "cpu1: 2 cores" in log and peak(s, 1.7, 3.0) > 1000 and " 0 overruns" in log
              and worst < 50, f"CS-1 plays eight voices, half of them on the second core; core 0's worst {worst} % of a block")
        # a unit whose second core misreads RAM (fm1-emu's model of the affected FM-1s): the screening in the
        # first audio blocks finds it, and CS-1 renders all eight voices on core 0
        s, log = run("cs1_flaky", keys, 4000, extra=("--flaky-core1", "1e-6", "--seed", "2"))
        worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
        check("cs1_flaky", "cpu1: misread RAM, 1 core" in log and "cs1: CS-1" in log and peak(s, 1.7, 3.0) > 1000
              and " 0 overruns" in log and worst < 90, f"a faulty second core: one core, CS-1 plays on; worst {worst} %")

    if "SCINTILLA" in names:
        # its heaviest factory patch, High Portamento (MEMORIES 69: program change into the DIN jack), an 8-note chord
        # struck every 200 ms: each attack sweeps the filter 114 semitones in 5 ms, new coefficients nearly every
        # sample (fm1-dock 0.7.1 went to 126 % of a block on such chords, and a real FM-1 stopped the module). Half
        # of the voices on the second core; then on a unit whose core 1 misreads, all of them on core 0
        chord = (14, 18, 21, 25, 28, 32, 35, 39)
        keys = pick("SCINTILLA") + ",1300:mc045" + "".join(f",{t}:{k},{t + 120}:{k}^" for t in range(1600, 3700, 200)
                                                             for k in chord)
        s, log = run("scintilla", keys, 4000)
        worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
        check("scintilla", "scintilla: SCINTILLA" in log and "cpu1: 2 cores" in log and peak(s, 1.7, 3.6) > 1000
              and " 0 overruns" in log and worst < 50,
              f"Scintilla strikes 8-note chords on High Portamento, half the voices on the second core; core 0's worst "
              f"{worst} % of a block")
        s1, log = run("scintilla_flaky", keys, 4000, extra=("--flaky-core1", "1e-6", "--seed", "2"))
        worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
        r0, r1 = rms_left(s, 1.7, 3.6), rms_left(s1, 1.7, 3.6)
        check("scintilla_flaky", "cpu1: misread RAM, 1 core" in log and " 0 overruns" in log and worst < 85
              and abs(r1 - r0) < 0.02 * r0, f"a faulty second core: all the voices on core 0 (worst {worst} %), as loud "
              f"({r1:.0f} / {r0:.0f} RMS: the main loop, slower, may post a note a block later)")

    if "FM-106" in names:
        # a six-note chord on preset 38 (A57, among the heaviest of the 128 on one core: bank select and program
        # change into the DIN jack); voices 3..5 on the second core, then on a unit whose core 1 misreads, all on core 0
        keys = pick("FM-106") + ",1400:mb00000,1450:mc026" + "".join(f",1800:{k},3200:{k}^" for k in (14, 18, 21, 25, 28, 32))
        s, log = run("fm106", keys, 3600)
        worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
        check("fm106", "fm106: FM-106" in log and "cpu1: 2 cores" in log and peak(s, 1.7, 3.0) > 1000 and " 0 overruns" in log
              and worst < 45, f"FM-106 plays six voices, three on the second core; core 0's worst {worst} % of a block")
        s1, log = run("fm106_flaky", keys, 3600, extra=("--flaky-core1", "1e-6", "--seed", "2"))
        worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
        check("fm106_flaky", "cpu1: misread RAM, 1 core" in log and " 0 overruns" in log and s1 == s and worst < 80,
              f"a faulty second core: all six voices on core 0 (worst {worst} %), the same sound")
        # a user preset across a power-off: SAVE (a short press) stores the sound in user slot 1 as "*A11 Brass"
        # (its banks), the host keeps the module and FM-106 its patch (records); the next power-on loads them
        flash = OUT / "fm106_flash.bin"
        flash.unlink(missing_ok=True)
        fw = ("--fwsc", str(SRC / "build" / "fm1-dock.fwsc"), "--flash", str(flash))
        run("fm106_save", pick("FM-106") + ",2400:9,2500:9^", 6000, extra=fw)
        _, log = run("fm106_saved", "", 3000, extra=fw)
        ram = (OUT / "fm106_saved" / "ram.bin").read_bytes()
        check("fm106_presets", "fm106: FM-106" in log and b"*A11 Brass" in ram,
              "FM-106's user preset is there after a power-off (its banks loaded back)")

    if "MONK-1" in names:
        def rms(s, t0, t1):
            seg = s[int(t0 * 44100) * 2:int(t1 * 44100) * 2]
            return (sum(x * x for x in seg) / max(1, len(seg))) ** 0.5
        k = pick("MONK-1")
        a, _ = run("monk_soft", k + ",1800:23@20,2900:23^", 3200)
        b, log = run("monk", k + ",1800:23@127,2900:23^", 3200)
        c, _ = run("monk_pressure", k + ",1800:23@20,2400:23~127,2900:23^", 3200)   # (past the key's settling)
        check("monk", "monk: MONK-1" in log and peak(b, 0.0, 1.5) == 0 and rms(b, 1.7, 2.1) > 1000 and
              " 0 overruns" in log, f"MONK-1 starts and a key sings (RMS {rms(b, 1.7, 2.1):.0f}), nothing before it")
        check("monk_velocity", rms(b, 1.7, 2.1) > 1.5 * rms(a, 1.7, 2.1),
              f"MONK-1 takes the host's velocity (RMS {rms(a, 1.7, 2.1):.0f} soft, {rms(b, 1.7, 2.1):.0f} hard)")
        n = int(2.1 * 44100) * 2
        check("monk_pressure", a[:n] == c[:n] and a[n:] != c[n:],
              "MONK-1's own pressure from the host's readings: pressing harder moves the vowel")
        # the heaviest: ten voices in unison (EDIT, KNOB1), two octaves up, G5, the vowel swept on ALGORITHM all along
        # (each pitch period's grain made anew)
        sweep = "".join(f",{2300 + 60 * i}:e1:{3 if i // 5 % 2 == 0 else -3}" for i in range(20))
        s, log = run("monk_heavy", k + ",1300:1,1350:1^,1450:1,1500:1^,1700:6,1750:6^,1900:e2:12,2200:40" + sweep +
                     ",3500:40^", 3700)
        worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
        check("monk_heavy", peak(s, 2.1, 3.3) > 1000 and " 0 overruns" in log and worst < 80,
              f"MONK-1 sings ten voices at G7, its vowel moving; worst {worst} % of a block")
        # the next singer (PRESETS), SAVE: its project in the module's bank, there after a power-off
        flash = OUT / "monk_flash.bin"
        flash.unlink(missing_ok=True)
        fw = ("--fwsc", str(SRC / "build" / "fm1-dock.fwsc"), "--flash", str(flash))
        run("monk_save", k + ",1800:e6:1,2400:9,2500:9^", 6000, extra=fw)
        _, log = run("monk_saved", "", 3000, extra=fw)
        ram = (OUT / "monk_saved" / "ram.bin").read_bytes()
        check("monk_saved", "monk: MONK-1" in log and b"MONK\x03\x00\x00\x00\x01" in ram,
              "MONK-1's singer is there after a power-off (its project loaded back from its bank)")

    if "FIMBA" in names:
        # the heaviest FiMba-1: Buzz, Wah and its rate (ENV: Body), Reverb, Size and Delay (FX: Space), Tape, Lo-fi,
        # Chorus and Filter (FX again: Color), Grains and their density (LFO: Grain), Worn (HOME twice: Character), each
        # turned quickly to its top; then all 16 tines at once and two chord rolls over them
        keys = pick("FIMBA") + (",1300:4,1350:4^,1450:e3:20,1600:e4:20,1750:e5:20,1950:2,2000:2^,2100:e2:20,2250:e3:20,"
                                "2400:e4:20,2600:2,2650:2^,2750:e2:20,2900:e3:20,3050:e4:20,3200:e5:10,3400:5,3450:5^,"
                                "3550:e2:20,3700:e4:20,3900:8,3950:8^,4050:8,4100:8^,4200:e2:20")
        keys += "".join(f",4600:{14 + k},6200:{14 + k}^" for k in (0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26))
        s, log = run("fimba", keys + ",5200:15,5300:15^,5400:17,5500:17^", 7000)
        worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
        check("fimba", "fimba: FiMba-1" in log and peak(s, 0.0, 4.0) == 0 and peak(s, 4.5, 6.5) > 1000 and
              " 0 overruns" in log and worst < 80, f"FiMba-1 plays 16 tines through all its effects; worst {worst} % of a block")
        # the keys' force: a pluck as hard as the key lands; its pressure covers the sound hole (the wah)
        k = pick("FIMBA")
        a, _ = run("fimba_soft", k + ",1800:23@20,2600:23^", 3000)
        b, _ = run("fimba_hard", k + ",1800:23@127,2600:23^", 3000)
        c, _ = run("fimba_pressure", k + ",1800:23@100,2000:23~127,2600:23^", 3000)
        d, _ = run("fimba_nopressure", k + ",1800:23@100,2600:23^", 3000)
        first = next((i for i, (x, y) in enumerate(zip(c, d)) if x != y), len(c)) / 88200
        diff = [x - y for x, y in zip(c, d)]
        check("fimba_force", peak(b, 1.5, 2.2) > 5 * peak(a, 1.5, 2.2) > 0 and 1.7 < first < 2.0 and
              rms_left(diff, 1.9, 2.3) > 0.3 * rms_left(d, 1.9, 2.3),
              f"FiMba-1 plucks at the key's velocity (peak {peak(a, 1.5, 2.2)} soft, {peak(b, 1.5, 2.2)} hard); its "
              f"pressure moves the wah from {first:.2f} s")

    if "GATOR" in names:
        # preset 1 (VIRTUAL ANLG) plays a held key; then HOME held + note key 15 picks the SPEECH model, HARM,
        # TIMBRE and MORPH turned up (a word, the heaviest of its 24 models here), and the key again says it
        keys = pick("GATOR") + ",1800:21,2300:8,2350:29,2400:29^,2450:8^,2500:e2:60,2510:e3:60,2520:e4:60,2600:21^," \
            "2650:21,3300:21^"
        s, log = run("gator", keys, 3500)
        worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
        check("gator", "gator: GATOR" in log and peak(s, 0.0, 1.5) == 0 and peak(s, 1.7, 2.1) > 1000 and
              peak(s, 2.5, 2.9) > 1000 and " 0 overruns" in log and worst < 40,
              f"GATOR (Plaits) plays a key, then its SPEECH model; worst {worst} % of a block")

    if "DUB-1" in names:
        # DUB-1 plays by itself once its launch screen is over (~1.4 s): PRESETS a click, Chamber (among its heaviest
        # pieces, root A); a key (G3) takes its note out of the scale (the project's pool, bit 10), PLAY rests it
        s, log = run("dub", pick("DUB-1") + ",3000:e6:1,4000:16,4200:16^,6000:12,6100:12^", 9500)
        worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
        ram = (OUT / "dub" / "ram.bin").read_bytes()
        p = ram.find(b"DUB1" + struct.pack("<I", 36))   # (project_t: magic, format 36, par[170], pool, leds, piece)
        pool, piece = struct.unpack_from("<HxB", ram, p + 348) if p >= 0 else (0, 0)
        check("dub", "dub: DUB-1" in log and peak(s, 0.0, 1.0) == 0 and peak(s, 3.0, 5.5) > 1000 and
              peak(s, 8.5, 9.2) < peak(s, 3.0, 5.5) // 20 and piece == 1 and pool == 0x1AD and " 0 overruns" in log
              and worst < 75, f"DUB-1 plays by itself, a key changes its scale (pool {pool:#x}), PLAY rests it; "
              f"worst {worst} % of a block")

    if "PROPHET" in names:
        prophet_cases()

    if "FMINI-1" in names:
        fmini_cases()

    if "FM6" in names:
        fm6_cases()
    if "ATEK303" in names:
        atek_cases()

    if "B3 ORGAN" in names:
        # Standard B (88 8000 000) under ten keys; then over MIDI every drawbar out (CC 70..78 at 0, as setBfree),
        # percussion, the Leslie fast, overdrive, reverb and vibrato, and a ten-note chord: its heaviest
        chord = "".join(f",1800:{k},3000:{k}^" for k in (14, 16, 18, 19, 21, 23, 25, 26, 28, 30))
        s, log = run("b3", pick("B3 ORGAN") + chord, 3600)
        worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
        check("b3", "b3: B3 ORGAN" in log and peak(s, 0.0, 1.5) == 0 and peak(s, 1.9, 2.9) > 1000 and
              peak(s, 3.4, 3.6) < peak(s, 1.9, 2.9) // 20 and " 0 overruns" in log and worst < 70,
              f"B3 ORGAN plays Standard B under ten keys and stops; worst {worst} % of a block")
        cc = [(70 + i, 0) for i in range(9)] + [(66, 127), (1, 127), (65, 127), (91, 127), (31, 127), (93, 127)]
        m = "".join(f",{1300 + 10 * j}:mb0{c:02x}{v:02x}" for j, (c, v) in enumerate(cc))
        m += "".join(f",{1800 + 20 * j}:m90{n:02x}64" for j, n in enumerate(range(48, 68, 2)))
        s, log = run("b3_full", pick("B3 ORGAN") + m, 3000)
        worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
        check("b3_full", peak(s, 2.1, 2.9) > 1000 and " 0 overruns" in log and worst < 90,
              f"B3 ORGAN over MIDI: all drawbars, percussion, Leslie fast, overdrive, reverb, vibrato, ten keys; "
              f"worst {worst} % of a block")
    if "NIAGARA" in names:
        niagara_cases()
    if "KEYS" in names:
        keys_cases()
    if "ARTIHIVE" in names:
        # DRONE: silent until PLAY, which starts all eight drones (four FM pairs, the delay, the compressor);
        # POLY (HOME on its first page): an eight-note chord on the keys; a MIDI note latches voice 4 (60 & 7)
        k = pick("ARTIHIVE")
        s, log = run("artihive", k + ",2000:12,2050:12^", 4500)
        worst = float(log.split("worst ")[1].split("(")[1].split(" %")[0])
        check("artihive", "artihive: ARTIHIVE" in log and peak(s, 0.5, 1.7) == 0 and peak(s, 2.6, 4.0) > 1000 and
              " 0 overruns" in log and worst < 80, f"ARTIHIVE: PLAY starts its eight drones; worst {worst} % of a block")
        s, log = run("artihive_poly", k + ",2000:8,2050:8^," + ",".join(f"2400:{14 + n}" for n in (0, 4, 7, 11, 12, 16, 19, 23)), 4000)
        check("artihive_poly", peak(s, 0.5, 2.1) == 0 and rms_left(s, 2.6, 3.6) > 300 and " 0 overruns" in log,
              f"ARTIHIVE in POLY: an eight-note chord on the keys (RMS {rms_left(s, 2.6, 3.6):.0f})")
        base = k + ",2000:12,2050:12^,2600:12,2650:12^"   # (all drones on, then off: their tails go on)
        a, _ = run("artihive_nomidi", base, 5000)
        b, _ = run("artihive_midi", base + ",3600:m903c64", 5000)
        d = [x - y for x, y in zip(a, b)]
        check("artihive_midi", rms_left(d, 2.0, 3.35) == 0 and rms_left(d, 3.6, 4.6) > 100,
              f"ARTIHIVE over MIDI: a note latches its voice (RMS {rms_left(d, 3.6, 4.6):.0f} more than without it)")

    # the keys' force (fm1-emu's pills on the analog rows; sense.c in the host): key 23 (note key 9)
    # is on row PA7, key 34 (note key 20) on the top row, which has no converter
    s, log = run("velocity", "1000:23@20,1400:23^,1800:23@127,2200:23^,2400:34@20,2700:34^", 2900)
    soft, hard, top = peak(s, 0.9, 1.3), peak(s, 1.7, 2.1), peak(s, 2.3, 2.6)
    check("velocity", 0 < soft < hard / 2 and top > soft,
          f"a sensed key plays at the force it lands with (sine peak {soft} soft, {hard} hard); the top row at 100 ({top})")

    if "CS-1" in names:
        def rms(s, t0, t1):
            seg = s[int(t0 * 44100) * 2:int(t1 * 44100) * 2]
            return (sum(x * x for x in seg) / max(1, len(seg))) ** 0.5
        k = pick("CS-1")
        a, _ = run("cs1_soft", k + ",1800:23@20,2600:23^", 3000)
        b, _ = run("cs1_hard", k + ",1800:23@127,2600:23^", 3000)
        c, log = run("cs1_pressure", k + ",1800:23@40,2000:23~127,2600:23^", 3000)
        d, _ = run("cs1_nopressure", k + ",1800:23@40,2600:23^", 3000)
        check("cs1_velocity", rms(b, 1.9, 2.3) > 1.5 * rms(a, 1.9, 2.3),
              f"CS-1 takes the host's velocity (RMS {rms(a, 1.9, 2.3):.0f} soft, {rms(b, 1.9, 2.3):.0f} hard)")
        check("cs1_pressure", rms(c, 2.1, 2.4) > 1.1 * rms(d, 2.1, 2.4),
              f"CS-1's own pressure from the host's readings (RMS {rms(d, 2.1, 2.4):.0f} held, {rms(c, 2.1, 2.4):.0f} pressed harder)")

    s, log = run("din_midi", "1000:m903c64,1500:m803c00,1700:m9045,1800:m40", 2500)
    check("din_midi", peak(s, 0.9, 1.2) > 1000 and peak(s, 1.4, 1.5) < peak(s, 0.9, 1.2) and peak(s, 1.7, 2.0) > 1000,
          "the DIN jack plays notes (and running status split across bytes)")

    # the master EQ (OPTIONS, EQ: the hardware EQ, fm1-emu's hweq.rs): ON and flat changes nothing; a band
    # changes a sine's level as tools/eq_table.py's response says, within 1 dB (the emulator's EQ is not the
    # chip's bit for bit: within a few f32 ulp)
    import eq_table
    keys, t = eq_keys()
    flat_off, _ = run("eq_off", keys + f",{t + 300}:14,{t + 1300}:14^", t + 1500)
    keys, t2 = eq_keys(on=True)
    flat_on, log = run("eq_flat", keys + f",{t2 + 300}:14,{t2 + 1300}:14^", t2 + 1500)
    a, b = onset_frames(flat_off), onset_frames(flat_on)
    n = min(len(a), len(b))
    on = [(OUT / c / "ram.bin").read_bytes()[sym("eq_on")] for c in ("eq_off", "eq_flat")]   # (and it was ON)
    check("eq_flat", n > 40000 and a[:n] == b[:n] and "EQ:" not in log and on == [0, 1],
          f"the EQ ON at 0 dB: the sine bit for bit as with it OFF ({n // 2} frames; eq_on {on})")
    for case, band, db, oct_, k in (("eq_low", 0, 12, 0, 0), ("eq_mid", 1, -12, 1, 18), ("eq_high", 2, -12, 2, 26)):
        f = 440 * 2 ** ((53 + 12 * oct_ + k - 69) / 12)
        octs = "".join(f",{300 + 100 * i}:1,{350 + 100 * i}:1^" for i in range(oct_))   # (OCT+ before the list)
        res = []
        for gains in ((0, 0, 0), tuple(db if i == band else 0 for i in range(3))):
            keys, t = eq_keys(*gains)
            s, log = run(case if any(gains) else case + "_ref", keys + octs + f",{t + 300}:{14 + k},{t + 1300}:{14 + k}^",
                         t + 1500)
            res.append(rms_left(s, (t + 600) / 1000, (t + 1200) / 1000))
        got = 20 * __import__("math").log10(res[1] / max(res[0], 1e-9))
        want = 20 * __import__("math").log10(eq_table.response(band, db, f))
        check(case, abs(got - want) < 1.0 and res[0] > 100,
              f"{eq_table.BANDS[band][0]} {db:+d} dB: a {f:.0f} Hz sine {got:+.2f} dB (the table's response {want:+.2f})")
    # the EQ heard while it is set: a module with a panel plays its keys in its draw(), which runs under the picker
    # (OBERVAVE, started by 3.6 s: HOME held for the picker, OPTIONS, EQ; a key held, MID turned to -12 dB
    # meanwhile, against the same without the turn)
    if "OBERVAVE" in [n for n, _, _ in slots()]:
        idx = 1 + [n for n, _, _ in slots()].index("OBERVAVE")
        page = pick("OBERVAVE") + f",4000:8,5700:8^,5900:e6:{1 + len(slots()) + O_EQ - idx},6000:3,6050:3^,6400:26,9400:26^"
        res = []
        for case, turn in (("eq_live_ref", ""), ("eq_live", ",7800:e3:-12")):
            s, log = run(case, page + turn, 9600)
            res.append((rms_left(s, 6.7, 7.7), rms_left(s, 8.2, 9.2)))
        got = 20 * __import__("math").log10(res[1][1] / max(res[0][1], 1e-9))
        check("eq_live", min(res[0]) > 100 and abs(res[1][0] - res[0][0]) < 0.03 * res[0][0] and got < -1.0,
              f"OBERVAVE under the EQ's page: a key plays (RMS {res[0][0]:.0f}), MID -12 dB meanwhile {got:+.2f} dB")

    # USB audio: the computer records what the speaker plays, for every module, sample for sample (fm1-live's
    # FM1_USB_WAV; the speaker's capture is the DAC's value >> 8, the USB sample >> 1). A render that ran the
    # ring dry would repeat frames and shift what follows.
    def usb_frames(case):
        w = wave.open(str(OUT / case / "usb.wav"))
        d = w.readframes(w.getnframes())
        return w.getframerate(), struct.unpack(f"<{len(d) // 2}h", d)

    def uac_glitches(case):
        # the firmware's own counts (usb.c `uac`: underruns, overruns, missed at +36) from the run's RAM
        m = re.search(r"^([0-9a-f]{8}) .*\suac$", (SRC / "build" / "fm1-dock.syms").read_text(), re.M)
        return struct.unpack_from("<3I", (OUT / case / "ram.bin").read_bytes(), int(m.group(1), 16) - 0x01C00000 + 36)

    def usb_same(s, u):
        # the share of USB frames, from its first sound on, that are the speaker's at one lag
        spk = list(zip(s[0::2], s[1::2]))
        usb = [(a >> 1, b >> 1) for a, b in zip(u[0::2], u[1::2])]
        st = next((i for i, (a, _) in enumerate(usb) if abs(a) > 200), None)
        if st is None:
            return 0.0, "no sound"
        key = usb[st:st + 64]
        lag = next((j - st for j in range(len(spk) - 64) if spk[j] == key[0] and spk[j:j + 64] == key), None)
        if lag is None:
            return 0.0, "not found in the speaker's"
        pairs = [(usb[k], spk[k + lag]) for k in range(st, len(usb)) if 0 <= k + lag < len(spk)]
        return sum(a == b for a, b in pairs) / max(1, len(pairs)), f"{len(pairs)} frames at one offset"

    probe = subprocess.run([str(Path(os.environ.get("FM1_LIVE", Path.home() / "work/fm1-emu/target/release/fm1-live"))),
                            "--help"], capture_output=True, text=True)
    if "FM1_USB_WAV" not in probe.stdout + probe.stderr:
        print("  skip usb_audio: this fm1-live does not record USB audio (FM1_USB_WAV)")
    else:
        # (the chord at 4 s: OberVave takes ~2.4 s to start after the picker)
        chord = "".join(f",4000:{k},5200:{k}^" for k in (14, 18, 21, 25, 28, 32))
        for name in names:
            case = "usb_" + re.sub(r"[^a-z0-9]+", "", name.lower())
            play = ",3000:12,3050:12^" if name == "ARTIHIVE" else ""   # (ARTIHIVE is silent until PLAY)
            s, log = run(case, pick(name) + play + chord, 6000, extra=("--usb",))
            rate, u = usb_frames(case)
            same, how = usb_same(s, u)
            g = uac_glitches(case)
            check(case, rate == 44100 and same == 1.0 and g == (0, 0, 0),
                  f"{name}: USB audio is the speaker's, {same * 100:.2f} % ({how}); ring underruns, overruns, missed {g}")
        s, log = run("usb_48k", pick(names[0]) + chord, 6000, extra=("--usb", "48000"))
        rate, u = usb_frames("usb_48k")         # (the rate fm1-live streamed at)
        frames = len(u) // 2                    # (the stream starts once the computer has configured it, ~0.35 s)
        g = uac_glitches("usb_48k")
        check("usb_48k", rate == 48000 and 48 * 5400 < frames <= 48 * 6000 and g == (0, 0, 0),
              f"a 48 kHz computer: {rate} Hz, {frames} frames in 6 s; ring underruns, overruns, missed {g}")

    # the host's MIDI OPTIONS, with a computer on USB (what the FM-1 sends: usb.c's ring in the run's RAM)
    if "FM1_USB_WAV" in probe.stdout + probe.stderr:
        keys, t = options([(O_LOCAL, 1)])       # LOCAL OFF: the keys send, the sine plays nothing
        s, log = run("local_off", keys + f",{t + 300}:14,{t + 700}:14^", t + 1200, extra=("--usb",))
        out, t0 = midi_out("local_off"), (t + 300) / 1000
        check("local_off", peak(s, t0 - 0.1, t0 + 0.6) == 0 and any(st == 0x90 and v for st, _, v in out) and
              any(st == 0x80 for st, _, _ in out), f"LOCAL OFF: a key sends its note on and off, the sine stays silent ({out})")
        if "CS-1" in names:                     # (a module that plays its own keys: the host takes them)
            cur = 1 + names.index("CS-1")
            keys, t = options([(O_LOCAL, 1)], at=1700, cur=cur, hold=True)
            s, log = run("local_off_panel", pick("CS-1") + "," + keys + f",{t + 300}:14,{t + 700}:14^", t + 1200,
                         extra=("--usb",))
            out, t0 = midi_out("local_off_panel"), (t + 300) / 1000
            check("local_off_panel", peak(s, t0 - 0.1, t0 + 0.6) < 50 and any(st == 0x90 and v for st, _, v in out),
                  f"LOCAL OFF in CS-1: its key goes out as a note, CS-1 plays nothing (peak {peak(s, t0 - 0.1, t0 + 0.6)})")

        keys, t = options([(O_MASTER_CC7, 1)])  # MASTER CC7: the pot sends CC 7 as it moves; its noise does not
        s, log = run("master_cc7", keys + f",{t + 300}:pot:200,{t + 500}:pot:203,{t + 700}:pot:900", t + 1000,
                     extra=("--usb",))
        cc = [v for st, d1, v in midi_out("master_cc7") if st == 0xB0 and d1 == 7]
        low = cc.index(200 >> 3) if 200 >> 3 in cc else -1   # (the reading glides to where the pot is: a CC a value)
        down, up = cc[:low + 1], cc[low + 1:]
        check("master_cc7", low >= 0 and abs(cc[-1] - (900 >> 3)) <= 1 and all(x > y for x, y in zip(down, down[1:])) and
              all(x < y for x, y in zip(up, up[1:])),
              f"MASTER CC7: the pot to 200, 203 (noise: nothing), 900 sends CC 7 down to {cc[low] if low >= 0 else '?'} "
              f"and up to {cc[-1] if cc else '?'}, each value once ({len(cc)} messages)")

        keys, t = options([(O_CH, 2)])          # MIDI CH 2: channel 1 is not heard, 2 is; the keys send on 2
        s, log = run("midi_ch", keys + f",{t + 300}:m903c64,{t + 600}:m803c00,{t + 900}:m913c64,{t + 1200}:m813c00,"
                     f"{t + 1500}:14,{t + 1700}:14^", t + 2000, extra=("--usb",))
        out, t0 = midi_out("midi_ch"), (t + 300) / 1000
        check("midi_ch", peak(s, t0 - 0.1, t0 + 0.3) == 0 and peak(s, t0 + 0.6, t0 + 0.9) > 1000 and
              any(st == 0x91 for st, _, _ in out) and not any(st == 0x90 for st, _, _ in out),
              f"MIDI CH 2: a channel 1 note is not heard, a channel 2 one plays; the key sends on channel 2 ({out})")

        res = {}
        for case, n in (("midi_thru", 1), ("midi_nothru", 0)):   # MIDI THRU: the DIN jack's note goes on to USB
            keys, t = options([(O_THRU, n)])    # (and plays); left OFF: it plays, nothing goes out
            s, log = run(case, keys + f",{t + 300}:m903c64,{t + 600}:m803c00", t + 1000, extra=("--usb",))
            res[case] = midi_out(case), peak(s, (t + 300) / 1000, (t + 600) / 1000)
        (out, p1), (out2, p2) = res["midi_thru"], res["midi_nothru"]
        check("midi_thru", (0x90, 0x3C, 0x64) in out and (0x80, 0x3C, 0) in out and (0x90, 0x3C, 0x64) not in out2 and
              p1 > 1000 and p2 > 1000, f"MIDI THRU: the DIN jack's note on and off go out on USB ({out}), and play; "
              f"OFF: not sent ({out2})")

        res = {}
        for case, n in (("knobs_cc", 1), ("knobs_off", 0)):   # KNOBS CC: KNOB1 up 5 detents, KNOB2 down 3, slowly;
            keys, t = options([(O_KNOBS, n)])   # then KNOB1 down fast, 30 ms apart (4 steps a detent; fm1-live
            # drops one of two clicks 10 ms apart); OFF: nothing goes out
            # (from 600 ms after OPTIONS: a detent in the picker's last pass, a few hundred ms in fm1-emu, is the list's)
            turns = "".join(f",{t + 600 + 100 * i}:e2:1" for i in range(5)) + \
                "".join(f",{t + 1200 + 100 * i}:e3:-1" for i in range(3)) + "".join(f",{t + 1600 + 30 * i}:e2:-1" for i in range(3))
            s, log = run(case, keys + turns, t + 1800, extra=("--usb",))
            res[case] = [m for m in midi_out(case) if m[0] == 0xB0]
        out, out2 = res["knobs_cc"], res["knobs_off"]
        check("knobs_cc", out == [(0xB0, 74, 65 + i) for i in range(5)] + [(0xB0, 71, 63 - i) for i in range(3)] +
              [(0xB0, 74, 68), (0xB0, 74, 64), (0xB0, 74, 60)] and out2 == [],
              f"KNOBS CC: KNOB1 sends CC 74 64 -> 69, KNOB2 CC 71 down to 61, fast four a detent ({out}); OFF: none ({out2})")

    if "CS-1" in names:
        # AFTERTOUCH: CS-1 holds a DIN note while poly pressure rises on it; FILTER (CC 74, its brilliance) and
        # MOD WHL (CC 1, its expression) change the sound from the first pressure on, not before; the same option
        # with no pressure sent is the reference (fm1-emu is deterministic)
        res = {}
        for mode, n in (("filter", 3), ("mod", 2)):
            for press in (False, True):
                keys, t = options([(O_AT, n)])
                idx = 1 + [nm for nm, _, _ in slots()].index("CS-1")
                keys += f",{t + 200}:8,{t + 250}:8^,{t + 400}:e6:{idx},{t + 600}:8,{t + 650}:8^,{t + 1200}:m903c64"
                if press:
                    keys += "".join(f",{t + 2000 + 40 * i}:ma03c{16 + 8 * i:02x}" for i in range(14))
                s, log = run(f"at_{mode}{'' if press else '_ref'}", keys, t + 3200)
                res[mode, press] = s, t / 1000
        why, ok = [], True
        for mode in ("filter", "mod"):
            (a, t0), (b, _) = res[mode, False], res[mode, True]
            d = [x - y for x, y in zip(a, b)]     # (the WAV starts ~0.2 s after boot: the note at t0 + 1.0 in it,
            pre, post = rms_left(d, t0 + 1.2, t0 + 1.75), rms_left(d, t0 + 2.0, t0 + 2.8)   # the pressure at 1.8)
            ok = ok and rms_left(a, t0 + 1.2, t0 + 1.75) > 300 and pre == 0 and post > 100
            why.append(f"{mode} {pre:.0f} before, {post:.0f} after")
        check("aftertouch", ok, "AFTERTOUCH: poly pressure as CC 74 (FILTER) and as CC 1 (MOD WHL) changes CS-1 from "
              f"the first pressure on, not before (RMS of the difference: {'; '.join(why)})")

    print("all passed" if not failures else f"FAILED: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
