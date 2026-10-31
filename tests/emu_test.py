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


O_CC7, O_MASTER_CC7, O_CH, O_LOCAL, O_EQ, O_KNOBS, O_THRU = range(7)   # the OPTIONS lines (dock.c), past the list's modules


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


LINE = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t(.*)$")
NEVER_RUN = {"wfe", "sevl", "sevg", "trigger"}   # (multicore events in library code; the host's debug trigger)


def decode_check():
    """every instruction of the host and the modules through the emulator's own decoder (fm1-emu
    --check-decode, next to fm1-live): what it cannot run fails here, not mid-test"""
    emu = Path(os.environ.get("FM1_LIVE", Path.home() / "work/fm1-emu/target/release/fm1-live")).with_name("fm1-emu")
    for dis in [SRC / "build" / "fm1-dock.dis", *sorted((SRC / "build" / "modules").glob("*/module.dis"))]:
        rows = [(m.group(2).split(), m.group(3).strip()) for m in map(LINE.match, dis.read_text().splitlines())
                if m and not m.group(3).startswith("<")]
        text = "\n".join("".join(b + (rows[i + 1][0] if i + 1 < len(rows) else ["00", "00"])) for i, (b, _) in enumerate(rows))
        with tempfile.NamedTemporaryFile("w", suffix=".txt") as f:
            f.write(text + "\n")
            f.flush()
            out = subprocess.run([str(emu), "--check-decode", f.name], capture_output=True, text=True).stdout.splitlines()
        name = dis.parent.name if dis.parent.name != "build" else "host"
        if len(out) != len(rows):
            check(f"decode {name}", False, f"{emu} has no --check-decode (an fm1-emu older than 2026-10-19?)")
            continue
        bad = sorted({asm for (b, asm), o in zip(rows, out) if o.split("|")[-1] != "ok" and asm.split()[0] not in NEVER_RUN})
        check(f"decode {name}", not bad, f"{len(rows)} instructions, all known to the emulator" if not bad else
              f"unknown to the emulator: {', '.join(bad[:6])}")


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
            s, log = run(case, pick(name) + chord, 6000, extra=("--usb",))
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
            turns = "".join(f",{t + 300 + 100 * i}:e2:1" for i in range(5)) + \
                "".join(f",{t + 900 + 100 * i}:e3:-1" for i in range(3)) + "".join(f",{t + 1300 + 30 * i}:e2:-1" for i in range(3))
            s, log = run(case, keys + turns, t + 1500, extra=("--usb",))
            res[case] = [m for m in midi_out(case) if m[0] == 0xB0]
        out, out2 = res["knobs_cc"], res["knobs_off"]
        check("knobs_cc", out == [(0xB0, 74, 65 + i) for i in range(5)] + [(0xB0, 71, 63 - i) for i in range(3)] +
              [(0xB0, 74, 68), (0xB0, 74, 64), (0xB0, 74, 60)] and out2 == [],
              f"KNOBS CC: KNOB1 sends CC 74 64 -> 69, KNOB2 CC 71 down to 61, fast four a detent ({out}); OFF: none ({out2})")

    print("all passed" if not failures else f"FAILED: {', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
