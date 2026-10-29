#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""fm1-dock on the computer.

  tools/dock.py emu [--bin build/fm1-dock_xip.bin | --fwsc build/fm1-dock.fwsc] [--ms 3000] [--keys SCRIPT]
                      [--deploy MOD.dock [--at MS]] [--flash FLASH.bin] [--usb [HZ]] [--out DIR]
                      [--flaky-core1 RATE [--seed N]]  (a unit whose second core misreads RAM)
      runs the firmware in fm1-emu's fm1-live (headless, JIT): DIR/audio.wav, DIR/panel.png
      (the screen at the end), DIR/ram.bin and DIR/log.txt (the host's log, crash record, stats).
      --fwsc boots the package as the FM-1 would (the flash past the image readable: deploys need
      it); --deploy sends a module into the DIN jack at MS (default 1000); --flash: the flash chip
      from the file if it is there, back to it at the end (the next run boots from it); --usb: a USB
      cable to a computer that records the FM-1's USB audio (at HZ, default 44100) into DIR/usb.wav
  tools/dock.py log RAM.bin       the host's NOINIT state from a RAM dump (from 0x01C00000)

On an FM-1 (USB-MIDI; python-rtmidi: uv run --with python-rtmidi tools/dock.py ...):
  tools/dock.py deploy MOD.dock [--port NAME] [--blind]   the module into its flash, then running
                                       (a boot firmware, tools/hbboot.py: into the boot slot, then started)
  tools/dock.py deploy MOD.dock --syx OUT.syx            the same as a SysEx file (send it slowly)
  tools/dock.py modules | delete NAME | select NAME | safe | logs   [--port NAME]
  tools/dock.py send FILE [--arg N] [--port NAME]        a file into the RAM of the module playing (FM-1 SID:
                                       a .sid tune, played at once; --arg the subtune); never into flash
  --blind: no answers (an interface into the DIN jack); the FM-1's screen shows how it went

SCRIPT is fm1-live's FM1_BENCH_KEYS: `ms:id` presses a key / button by id (`ms:id^` releases;
note keys are 14 + k, k = 0 for F3 .. 26 for G5; buttons OCT- 0, OCT+ 1, HOME 8, SEL 3, PLAY 12),
`ms:eN:clicks` turns encoder N (0 SELECT, 2..5 KNOB1..4, 6 PRESETS), `ms:mHEX` sends MIDI bytes
into the DIN jack (`1000:m903c64`). FM1_LIVE points at fm1-live (default ~/work/fm1-emu).
"""
import argparse
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
RAM_BASE = 0x01C00000
NV_MAGIC = 0x564E4248
LOG_SIZE = 2048
SLOTS = 32


def live():
    p = Path(os.environ.get("FM1_LIVE", Path.home() / "work/fm1-emu/target/release/fm1-live"))
    if not p.exists():
        raise SystemExit(f"no fm1-live at {p} (build fm1-emu: cargo build --release, or set FM1_LIVE)")
    return p


def nv_addr(ram):
    syms = SRC / "build" / "fm1-dock.syms"
    if syms.exists():
        m = re.search(r"^([0-9a-f]{8}) .*\shb_nv$", syms.read_text(), re.M)
        if m:
            return int(m.group(1), 16) - RAM_BASE
    i = ram.find(struct.pack("<I", NV_MAGIC))
    return i if i >= 0 else None


def read_log(ram):
    o = nv_addr(ram)
    if o is None or struct.unpack_from("<I", ram, o)[0] != NV_MAGIC:
        return "no fm1-dock state in this RAM dump"
    (log_w,) = struct.unpack_from("<I", ram, o + 4)
    ring = ram[o + 8:o + 8 + LOG_SIZE]
    if log_w <= LOG_SIZE:
        text = ring[:log_w]
    else:
        p = log_w % LOG_SIZE
        text = ring[p:] + ring[:p]
    q = o + 8 + LOG_SIZE
    cmagic, vec, pc, rets, module, ms = struct.unpack_from("<6I", ram, q)
    active, = struct.unpack_from("<I", ram, q + 24)
    smod, last, worst, blocks, overruns = struct.unpack_from("<5I", ram, q + 28)
    strikes = ram[q + 48:q + 48 + SLOTS + 1]
    out = [text.decode("latin-1").rstrip("\n")]
    out.append("---")
    if cmagic == 0x48535243:
        out.append(f"last crash: pc {pc:#010x} vec {vec} rets {rets:#010x} module {module - 1 if module else '-'} at {ms} ms")
    block = 256 * 1_000_000 // 44100 * 24
    out.append(f"active module {active - 1 if active else '-'}; render: last {last} ticks ({100 * last / block:.1f} %), "
               f"worst {worst} ({100 * worst / block:.1f} %), {overruns} overruns, {blocks} blocks")
    s = [f"{i}:{n}" for i, n in enumerate(strikes) if n]
    out.append("strikes: " + (" ".join(s) if s else "none"))
    c = q + 48 + (SLOTS + 1 + 3) // 4 * 4         # the second core (firmware/src/cpu1.c), after the strikes
    c1magic, c1state, c1last, c1worst = struct.unpack_from("<4I", ram, c)
    if c1magic == 0x31555043:
        import hbdeploy
        out.append(f"second core: {hbdeploy.CPU1[c1state] if c1state < len(hbdeploy.CPU1) else c1state}; a job: "
                   f"last {c1last} ticks ({100 * c1last / block:.1f} %), worst {c1worst} ({100 * c1worst / block:.1f} %)")
    return "\n".join(out)


def emu(a):
    out = Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    fw = ["--fwsc", str(Path(a.fwsc).resolve())] if a.fwsc else ["--bin", str(Path(a.bin).resolve())]
    keys = [k for k in (a.keys or "").split(",") if k]
    if a.deploy:
        import hbdeploy
        entries, done = hbdeploy.din_script(Path(a.deploy).read_bytes(), a.at)
        keys += entries
        print(f"deploy {a.deploy}: COMMIT in by {done:.0f} ms")
    env = dict(os.environ, FM1_BENCH_KEYS=",".join(keys))
    fw += ["--flaky-core1", str(a.flaky_core1), "--seed", str(a.seed)] if a.flaky_core1 else []
    with tempfile.TemporaryDirectory() as wd:
        (Path(wd) / "work").mkdir()
        raw = Path(wd) / "audio.raw"
        flash = []
        if a.flash:                             # both runs from the same flash; the second's is kept
            import shutil
            for k in ("bench", "shot"):
                f = Path(wd) / f"flash_{k}.bin"
                if Path(a.flash).exists():
                    shutil.copy(a.flash, f)
                flash.append(["--flash", str(f)])
        else:
            flash = [[], []]
        usb, usb_env, shot_env = [], {}, {}
        if a.usb:                               # (both runs record: the first's sound is kept, the second's RAM)
            usb = ["--usb"]
            usb_env = {"FM1_USB_WAV": str(Path(wd) / "usb.raw"), "FM1_USB_RATE": str(a.usb)}
            shot_env = dict(usb_env, FM1_USB_WAV=str(Path(wd) / "usb_shot.raw"))
        r = subprocess.run([live(), *fw, "--bench", str(a.ms), *flash[0], *usb], cwd=wd, capture_output=True, text=True,
                           env=dict(env, FM1_BENCH_WAV=str(raw), **usb_env))
        if r.returncode:
            sys.stderr.write(r.stdout + r.stderr)
            raise SystemExit("fm1-live --bench failed")
        pcm = raw.read_bytes()
        with wave.open(str(out / "audio.wav"), "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(44100)
            w.writeframes(pcm)
        if a.usb:
            got = Path(wd) / "usb.raw"
            m = re.search(r"^usb audio: .* at (\d+) Hz", r.stdout, re.M)
            if not got.exists() or not m:
                raise SystemExit("fm1-live does not record USB audio (FM1_USB_WAV): an older fm1-emu")
            print(m.group(0).split(" -> ")[0])
            with wave.open(str(out / "usb.wav"), "wb") as w:
                w.setnchannels(2)
                w.setsampwidth(2)
                w.setframerate(int(m.group(1)))   # (the rate it streamed at: the device's, if not the one asked for)
                w.writeframes(got.read_bytes())
        r = subprocess.run([live(), *fw, "--shot", str(a.ms), *flash[1], *usb], cwd=wd, capture_output=True, text=True,
                           env=dict(env, FM1_DUMPRAM=str(out / "ram.bin"), **shot_env))
        if r.returncode:
            sys.stderr.write(r.stdout + r.stderr)
            raise SystemExit("fm1-live --shot failed")
        if "halt" in r.stdout + r.stderr:
            print((r.stdout + r.stderr).strip())
        for ln in r.stdout.splitlines():
            if ln.startswith("flash:"):
                print(ln)
        if a.flash:
            import shutil
            shutil.copy(flash[1][1], a.flash)
        ppm = Path(wd) / "work" / "panel.ppm"
        try:
            from PIL import Image
            img = Image.open(ppm)
            img.save(out / "panel.png")
            lcd = find_lcd(img)
            if lcd:
                lcd.save(out / "screen.png")
        except ImportError:
            ppm.replace(out / "panel.ppm")
    log = read_log((out / "ram.bin").read_bytes())
    (out / "log.txt").write_text(log + "\n")
    s = struct.unpack(f"<{len(pcm) // 2}h", pcm)
    step = 44100 // 4
    peaks = [max((abs(x) for x in s[2 * t:2 * (t + step)]), default=0) for t in range(0, len(s) // 2, step)]
    print(f"{out}: {len(s) // 2 / 44100:.2f} s of audio; peak per 0.25 s: {' '.join(str(p) for p in peaks)}")
    print(log)


def find_lcd(img):
    """the 240 x 240 screen inside the panel picture, as fm1-live draws it (its frame is fixed)"""
    try:
        sys.path.insert(0, str(Path.home() / "work/fm1-emu/tools"))
    except Exception:
        return None
    return None


BOOT_WARNING = """This is a boot firmware: it takes over the FM-1 at every power-on, with its own USB.
If its USB does not answer, no installer reaches the FM-1 while it runs. The way back to fm1-dock:
  - hold HOME while switching on (keep holding while STARTING shows), or
  - switch off while STARTING shows, twice in a row: the third start stays in fm1-dock.
Deploy it only if you know it answers the installers (test it in fm1-emu first: tools/dock.py emu).
Add --boot to deploy it anyway."""


def device(a):
    import hbdeploy as hd
    if a.cmd == "deploy" and a.syx:
        blob = Path(a.module).read_bytes()
        frames = [hd.frame(c, i, p) for i, (c, p) in enumerate(hd.deploy_frames(blob))]
        Path(a.syx).write_bytes(b"".join(frames))
        print(f"{a.syx}: {len(frames)} SysEx messages; send them with a pause of 0.4 s after the first and "
              f"before the last (and a few ms between the others over DIN)")
        return
    p = hd.Port(a.port, blind=getattr(a, "blind", False))
    if a.cmd == "deploy":
        blob = Path(a.module).read_bytes()
        if hd.is_boot(blob) and not a.boot:
            raise SystemExit(BOOT_WARNING)
        hd.deploy(p, blob)
    elif a.cmd == "modules":
        h = hd.hello(p)
        for m in hd.modules(p):
            mark = ">" if m["index"] == h["cur"] else " "
            size = f"{m['size'] // 1024:4d} KiB v{m['version']}" if m["at"] else "          "
            print(f"{mark} {m['index']:2d} {m['name']:<16} {m['kind']:<9} {size}  {m['state']}")
        print(f"free for deploys: {h['free'] // 1024} KiB (largest gap {h['largest'] // 1024} KiB); "
              f"render worst {100 * h['worst'] / h['block']:.0f} % of a block, {h['overruns']} overruns")
        if "cpu1" in h:
            print(f"second core: {h['cpu1']}; a job's last {100 * h['cpu1_job'] / h['block']:.0f} %, worst "
                  f"{100 * h['cpu1_worst'] / h['block']:.0f} % of a block; {h['cpu1_timeouts']} timeouts, "
                  f"{h['cpu1_stack_free']} B of its stack never used")
        if h["crash_pc"]:
            print(f"last crash: pc {h['crash_pc']:#010x} in module {h['crash_module'] - 1 if h['crash_module'] else '-'}")
    elif a.cmd == "logs":
        text, _ = hd.log_text(p)
        print(text.rstrip("\n"))
    elif a.cmd == "send":
        blob = Path(a.file).read_bytes()
        bad = hd.send_file(p, blob, a.arg)
        if bad:
            step, d = bad
            why = f"the module's error {d[1]}" if d and d[0] == 11 else hd.status(d)
            raise SystemExit(f"{a.file}: {why} (at {step})")
        print(f"{a.file}: sent, {len(blob)} bytes (nothing written to flash)")
    else:
        cmd = {"delete": hd.DELETE, "select": hd.SELECT, "safe": hd.SAFE}[a.cmd]
        d = p.ask(cmd, hd.name24(a.name) if a.cmd != "safe" else b"", timeout=10.0)
        print(f"{a.cmd}: {hd.status(d)}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("emu")
    e.add_argument("--bin", default=str(SRC / "build" / "fm1-dock_xip.bin"))
    e.add_argument("--fwsc")
    e.add_argument("--ms", type=int, default=3000)
    e.add_argument("--keys", default="")
    e.add_argument("--deploy")
    e.add_argument("--at", type=int, default=1000)
    e.add_argument("--flash")
    e.add_argument("--usb", type=int, nargs="?", const=44100, metavar="HZ",
                   help="a USB cable to a computer that records the USB audio (HZ: 44100 or 48000) into OUT/usb.wav")
    e.add_argument("--flaky-core1", type=float, help="fm1-live's faulty second core: its RAM reads wrong at this rate")
    e.add_argument("--seed", type=int, default=1, help="--flaky-core1's random seed")
    e.add_argument("--out", default=str(SRC / "build" / "emu"))
    lg = sub.add_parser("log")
    lg.add_argument("ram")
    d = sub.add_parser("deploy")
    d.add_argument("module")
    d.add_argument("--syx")
    d.add_argument("--blind", action="store_true")
    d.add_argument("--boot", action="store_true", help="yes, a boot firmware (it takes over at power-on)")
    sd = sub.add_parser("send", help="a file to the module playing, into its RAM (FM-1 SID: a .sid tune, played at once)")
    sd.add_argument("file")
    sd.add_argument("--arg", type=int, default=0, help="for the module (FM-1 SID: the subtune, 0: the tune's own)")
    for name in ("modules", "delete", "select", "safe", "logs"):
        c = sub.add_parser(name)
        if name in ("delete", "select"):
            c.add_argument("name")
    for c in sub.choices.values():
        if c.prog.split()[-1] not in ("emu", "log"):
            c.add_argument("--port")
    a = ap.parse_args()
    if a.cmd == "emu":
        emu(a)
    elif a.cmd == "log":
        print(read_log(Path(a.ram).read_bytes()))
    else:
        device(a)


if __name__ == "__main__":
    main()
