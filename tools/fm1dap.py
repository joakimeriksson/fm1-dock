#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""fm1-dock Debug Adapter Protocol server: the FM-1's diagnostics in your IDE.

  uv run --with python-rtmidi tools/fm1dap.py [--port NAME]
  tools/fm1dap.py --emu                          (boot emulator, attach to RAM dump)
  tools/fm1dap.py --ram build/emu/ram.bin        (from an emulator RAM dump)
  tools/fm1dap.py --mock                         (synthetic data, no hardware needed)
  tools/fm1dap.py --emu --console                (human-readable output, no DAP)

VS Code: .vscode/launch.json with type "fm1-dock". IntelliJ: LSP4IJ plugin with
Debug Mode = Launch. Neovim: nvim-dap config. See docs/DAP.md.

The adapter speaks DAP on stdin/stdout and talks to the FM-1 over USB-MIDI SysEx
(tools/hbdeploy.py), or reads from a RAM dump (--ram), or uses synthetic data (--mock).

What it shows: the log ring (Debug Console), crash record and render stats (Variables),
the module list, and arbitrary memory reads (Memory view via the peek probe).
What it cannot do: breakpoints, stepping, source mapping (the pi32v2 SoC has no
hardware debug port and no public DWARF).

See docs/DAP.md for setup instructions."""
import argparse
import base64
import json
import os
import struct
import sys
import threading
import time
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SRC / "tools"))

RAM_BASE = 0x01C00000
NV_MAGIC = 0x564E4248
CRASH_MAGIC = 0x48535243
LOG_SIZE = 2048
SLOTS = 32

SEQ_LOCK = threading.Lock()
SEQ = 0

THREAD_ID = 1
FRAME_ID = 1
SCOPE_CRASH = 1000
SCOPE_STATS = 2000
SCOPE_MODULES = 3000

KIND = ["built in", "image", "deployed", "boot"]


# ---------------------------------------------------------------------------
# DAP transport: Content-Length framing over stdio
# ---------------------------------------------------------------------------

def read_message(stream):
    headers = {}
    while True:
        line = stream.readline()
        if not line:
            return None
        line = line.strip()
        if not line:
            break
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip()] = v.strip()
    length = int(headers.get("Content-Length", 0))
    if length == 0:
        return None
    body = stream.read(length)
    return json.loads(body)


_stdout = None


def _get_stdout():
    global _stdout
    if _stdout is None:
        _stdout = sys.stdout.buffer if hasattr(sys.stdout, "buffer") else sys.stdout
    return _stdout


def send_message(msg):
    global SEQ
    with SEQ_LOCK:
        SEQ += 1
        msg["seq"] = SEQ
    body = json.dumps(msg, separators=(",", ":")).encode("utf-8")
    header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
    out = _get_stdout()
    out.write(header + body)
    out.flush()


def respond(req, body=None, success=True, message=None):
    msg = {
        "type": "response",
        "request_seq": req["seq"],
        "command": req["command"],
        "success": success,
    }
    if body is not None:
        msg["body"] = body
    if message is not None:
        msg["message"] = message
    send_message(msg)


def event(name, body=None):
    msg = {"type": "event", "event": name}
    if body is not None:
        msg["body"] = body
    send_message(msg)


# ---------------------------------------------------------------------------
# Session backends
# ---------------------------------------------------------------------------

class Session:
    """Live connection to an FM-1 over USB-MIDI SysEx."""

    def __init__(self, port_name=None):
        import hbdeploy as hd
        self._hd = hd
        self.port = hd.Port(port_name)
        self.hello_data = {}
        self.module_list = []
        self.log_pos = 0
        self.running = False
        self.lock = threading.Lock()

    def refresh(self):
        with self.lock:
            self.hello_data = self._hd.hello(self.port)
        return self.hello_data

    def refresh_modules(self):
        with self.lock:
            self.module_list = self._hd.modules(self.port)
        return self.module_list

    def read_log_new(self):
        hd = self._hd
        h = self.hello_data
        w = h.get("log_w", 0)
        if w <= self.log_pos:
            return []
        lines = []
        frm = self.log_pos
        with self.lock:
            while frm < w:
                d = self.port.ask(hd.LOG, struct.pack("<I", frm))
                _, at = struct.unpack_from("<II", d, 4)
                chunk = d[12:]
                if not chunk:
                    break
                text = bytes(chunk).decode("latin-1", errors="replace")
                lines.extend(text.splitlines())
                frm = at + len(chunk)
        self.log_pos = frm
        return lines

    def peek(self, address, byte_count=64):
        hd = self._hd
        word_count = (byte_count + 3) // 4
        if word_count > 256:
            word_count = 256
        run_id = int(time.time()) & 0x7FFFFFFF
        blob = _build_peek()
        if blob is None:
            return None
        arg = struct.pack("<II", address & 0xFFFFFFFC, word_count)
        frames = hd.deploy_frames(blob, (hd.RUN, hd.run_payload(run_id, arg)))
        with self.lock:
            for cmd, payload in frames[:-1]:
                d = self.port.ask(cmd, payload, timeout=1.0, tries=4)
                if d[0]:
                    return None
            cmd, payload = frames[-1]
            seq = self.port.send(cmd, payload)
            for c, d in self.port.replies(seq, 3.0):
                if c == hd.RUN or c == 0x3F:
                    if d[0]:
                        return None
                    return self._read_result()
        return None

    def _read_result(self):
        hd = self._hd
        out, frm = b"", 0
        while True:
            d = self.port.ask(hd.RESULT, struct.pack("<I", frm))
            _, _, _, out_n, _, at = struct.unpack_from("<IiIIII", d, 4)
            chunk = d[28:]
            out += chunk
            frm = at + len(chunk)
            if not chunk or frm >= out_n:
                break
        import probe as pr
        res = pr.decode(out[:out_n])
        words = res.get("words", [])
        raw = b""
        for w in words:
            raw += struct.pack("<I", int(w, 16))
        return raw

    def close(self):
        self.running = False


class RamSession:
    """Offline session from an emulator RAM dump (tools/dock.py emu produces build/emu/ram.bin)."""

    def __init__(self, ram_path):
        self.ram = Path(ram_path).read_bytes()
        self.nv_off = self._find_nv()
        self.hello_data = {}
        self.module_list = []
        self.log_pos = 0
        self.running = False
        self.lock = threading.Lock()

    def _find_nv(self):
        syms = SRC / "build" / "fm1-dock.syms"
        if syms.exists():
            import re
            m = re.search(r"^([0-9a-f]{8}) .*\shb_nv$", syms.read_text(), re.M)
            if m:
                return int(m.group(1), 16) - RAM_BASE
        i = self.ram.find(struct.pack("<I", NV_MAGIC))
        return i if i >= 0 else None

    def refresh(self):
        if self.nv_off is None:
            self.hello_data = {}
            return self.hello_data
        o = self.nv_off
        log_w, = struct.unpack_from("<I", self.ram, o + 4)
        q = o + 8 + LOG_SIZE
        cmagic, vec, pc, rets, module, ms = struct.unpack_from("<6I", self.ram, q)
        active, = struct.unpack_from("<I", self.ram, q + 24)
        _, last, worst, blocks, overruns = struct.unpack_from("<5I", self.ram, q + 28)
        block_ticks = 256 * 1_000_000 // 44100 * 24
        self.hello_data = {
            "proto": 0, "abi": 0, "n": 0, "cur": active, "receiving": 0,
            "largest": 0, "free": 0, "max_file": 0,
            "crash_pc": pc if cmagic == CRASH_MAGIC else 0,
            "crash_module": module if cmagic == CRASH_MAGIC else 0,
            "worst": worst, "overruns": overruns, "log_w": log_w,
            "block": block_ticks,
        }
        return self.hello_data

    def refresh_modules(self):
        self.module_list = [{"index": 0, "name": "(ram dump)", "state": "ok",
                             "kind": 0, "version": 0, "size": 0, "at": 0}]
        return self.module_list

    def read_log_new(self):
        if self.nv_off is None:
            return []
        o = self.nv_off
        log_w, = struct.unpack_from("<I", self.ram, o + 4)
        ring = self.ram[o + 8:o + 8 + LOG_SIZE]
        if log_w <= self.log_pos:
            return []
        if log_w <= LOG_SIZE:
            text = ring[self.log_pos:log_w]
        else:
            start = max(self.log_pos, log_w - LOG_SIZE)
            text = bytearray()
            for i in range(start, log_w):
                text.append(ring[i % LOG_SIZE])
        self.log_pos = log_w
        return text.decode("latin-1", errors="replace").splitlines()

    def peek(self, address, byte_count=64):
        offset = address - RAM_BASE
        if offset < 0 or offset + byte_count > len(self.ram):
            return None
        return self.ram[offset:offset + byte_count]

    def close(self):
        self.running = False


class MockSession:
    """Synthetic data for testing the DAP protocol without hardware or an emulator."""

    def __init__(self):
        self.hello_data = {}
        self.module_list = []
        self.log_pos = 0
        self.running = False
        self.lock = threading.Lock()
        self._log_lines = [
            "0 boot",
            "12 slots: 3",
            "15 load sine ok",
            "20 load CS-1 ok",
            "25 load OberVave ok",
            "30 select CS-1",
            "35 cs1: CS-1 ready",
            "1200 render worst 4200 ticks",
        ]
        self._log_cursor = 0
        self._mock_ram = bytes(range(256)) * 4

    def refresh(self):
        self.hello_data = {
            "proto": 5, "abi": 12, "n": 3, "cur": 2, "receiving": 0,
            "largest": 65536, "free": 131072, "max_file": 65536,
            "crash_pc": 0, "crash_module": 0,
            "worst": 3800, "overruns": 0, "log_w": 200,
            "block": 139319,
        }
        return self.hello_data

    def refresh_modules(self):
        self.module_list = [
            {"index": 0, "name": "sine", "state": "ok", "kind": 0, "version": 1, "size": 2048, "at": 0x100000},
            {"index": 1, "name": "CS-1", "state": "ok", "kind": 2, "version": 3, "size": 32768, "at": 0x110000},
            {"index": 2, "name": "OberVave", "state": "ok", "kind": 2, "version": 2, "size": 28672, "at": 0x120000},
        ]
        return self.module_list

    def read_log_new(self):
        if self._log_cursor >= len(self._log_lines):
            return []
        lines = self._log_lines[self._log_cursor:]
        self._log_cursor = len(self._log_lines)
        return lines

    def peek(self, address, byte_count=64):
        return self._mock_ram[:byte_count]

    def close(self):
        self.running = False


_peek_blob = None


def _build_peek():
    global _peek_blob
    if _peek_blob is not None:
        return _peek_blob
    try:
        import hbmod
        import contextlib
        with contextlib.redirect_stdout(sys.stderr):
            _peek_blob = hbmod.build_module(SRC / "probes" / "peek.c").read_bytes()
        return _peek_blob
    except Exception:
        return None


# ---------------------------------------------------------------------------
# DAP request handlers
# ---------------------------------------------------------------------------

def handle_initialize(session, req):
    respond(req, body={
        "supportsReadMemoryRequest": True,
        "supportsMemoryReferences": True,
        "supportsEvaluateForHovers": False,
        "supportsSteppingGranularity": False,
        "supportsSetVariable": False,
        "supportsConfigurationDoneRequest": True,
        "supportsSingleThreadExecutionRequests": False,
    })
    event("initialized")


def handle_attach(session_box, req):
    args = req.get("arguments", {})
    try:
        mode = _cli_args.get("mode", "usb")
        if mode == "mock":
            s = MockSession()
        elif mode == "ram":
            s = RamSession(_cli_args["ram"])
        else:
            port_name = args.get("port") or _cli_args.get("port") or None
            s = Session(port_name)
        s.refresh()
        s.refresh_modules()
        s.log_pos = max(0, s.hello_data.get("log_w", 0) - 2048)
        s.running = True
        session_box[0] = s
        respond(req)
        h = s.hello_data

        # Emit boot info and log to Debug Console so the user sees something
        mode_label = {"mock": "mock", "ram": f"RAM dump: {_cli_args.get('ram', '?')}"}.get(
            _cli_args.get("mode", "usb"), f"USB-MIDI")
        event("output", {"category": "console",
                         "output": f"[fm1-dock] connected via {mode_label}\n"})
        event("output", {"category": "console",
                         "output": f"[fm1-dock] worst render: {h.get('worst', '?')} ticks, "
                                   f"overruns: {h.get('overruns', '?')}, "
                                   f"modules: {h.get('n', '?')}\n"})
        saved_pos = s.log_pos
        lines = s.read_log_new()
        s.log_pos = saved_pos
        if lines:
            event("output", {"category": "console",
                             "output": "[fm1-dock] boot log:\n"})
            for line in lines:
                event("output", {"category": "stdout", "output": line + "\n"})

        if h.get("crash_pc", 0):
            event("stopped", {
                "reason": "exception",
                "threadId": THREAD_ID,
                "description": f"Crash at PC {h['crash_pc']:#010x}",
                "text": f"pc={h['crash_pc']:#010x} module={h.get('crash_module', 0)}",
            })
        else:
            event("stopped", {
                "reason": "entry",
                "threadId": THREAD_ID,
                "description": "Attached to FM-1",
            })
    except (SystemExit, Exception) as e:
        respond(req, success=False, message=str(e))


def handle_configuration_done(session, req):
    respond(req)


def handle_threads(session, req):
    respond(req, body={"threads": [{"id": THREAD_ID, "name": "FM-1"}]})


def handle_stack_trace(session, req):
    h = session.hello_data if session else {}
    if h.get("crash_pc", 0):
        name = f"CRASH @ {h['crash_pc']:#010x}"
    else:
        name = "fm1-dock (running)"
    frames = [{
        "id": FRAME_ID,
        "name": name,
        "line": 0,
        "column": 0,
        "source": {"name": "fm1-dock", "presentationHint": "deemphasize"},
    }]
    respond(req, body={"stackFrames": frames, "totalFrames": len(frames)})


def handle_scopes(session, req):
    scopes = [
        {"name": "Crash Record", "variablesReference": SCOPE_CRASH,
         "expensive": False, "presentationHint": "registers"},
        {"name": "Render Stats", "variablesReference": SCOPE_STATS,
         "expensive": False},
        {"name": "Modules", "variablesReference": SCOPE_MODULES,
         "expensive": True},
    ]
    respond(req, body={"scopes": scopes})


def handle_variables(session, req):
    ref = req["arguments"]["variablesReference"]
    variables = []
    if session:
        session.refresh()
    h = session.hello_data if session else {}

    if ref == SCOPE_CRASH:
        pc = h.get("crash_pc", 0)
        mod = h.get("crash_module", 0)
        variables = [
            _var("crash_pc", f"{pc:#010x}" if pc else "none", "uint32",
                 mem_ref=f"{pc:#x}" if pc else None),
            _var("crash_module", str(mod - 1) if mod else "none", "uint32"),
        ]
    elif ref == SCOPE_STATS:
        block = h.get("block", 1) or 1
        worst = h.get("worst", 0)
        pct = f"{100 * worst / block:.1f}" if block else "?"
        variables = [
            _var("worst_render_%", pct, "string"),
            _var("overruns", str(h.get("overruns", 0)), "uint32"),
            _var("free_flash_KiB", str(h.get("free", 0) // 1024), "uint32"),
            _var("largest_gap_KiB", str(h.get("largest", 0) // 1024), "uint32"),
            _var("log_w", str(h.get("log_w", 0)), "uint32"),
            _var("proto", str(h.get("proto", 0)), "uint8"),
            _var("abi", str(h.get("abi", 0)), "uint8"),
            _var("current_module", str(h.get("cur", 0)), "uint8"),
            _var("ram_base", f"{RAM_BASE:#010x}", "uint32", mem_ref=f"{RAM_BASE:#x}"),
        ]
    elif ref == SCOPE_MODULES:
        if session:
            session.refresh_modules()
        cur = h.get("cur", -1)
        for m in (session.module_list if session else []):
            prefix = "> " if m["index"] == cur else "  "
            kind = KIND[m["kind"]] if m["kind"] < len(KIND) else str(m["kind"])
            at = m.get("at", 0)
            val = f"{kind} v{m['version']} {m['size'] // 1024} KiB [{m['state']}]"
            variables.append(_var(f"{prefix}{m['name']}", val, "module",
                                  mem_ref=f"{at:#x}" if at else None))

    respond(req, body={"variables": variables})


def _var(name, value, type_name="string", mem_ref=None):
    v = {"name": name, "value": value, "type": type_name, "variablesReference": 0}
    if mem_ref is not None:
        v["memoryReference"] = mem_ref
    return v


def handle_read_memory(session, req):
    args = req["arguments"]
    addr = int(args["memoryReference"], 0)
    offset = args.get("offset", 0)
    count = args.get("count", 64)
    addr += offset
    if not session:
        respond(req, success=False, message="not attached")
        return
    data = session.peek(addr, count)
    if data is None:
        respond(req, success=False, message="peek probe failed (is the toolchain available?)")
        return
    respond(req, body={
        "address": f"{addr:#x}",
        "data": base64.b64encode(data[:count]).decode("ascii"),
    })


def handle_evaluate(session, req):
    expr = req["arguments"].get("expression", "").strip()

    if expr.startswith("peek ") or expr.startswith("peek\t"):
        parts = expr.split()
        addr = int(parts[1], 0) if len(parts) > 1 else 0
        n = int(parts[2], 0) if len(parts) > 2 else 16
        data = session.peek(addr, n * 4) if session else None
        if data:
            words = struct.unpack(f"<{len(data) // 4}I", data)
            lines = [f"{addr + i * 4:#010x}: {w:#010x}" for i, w in enumerate(words)]
            respond(req, body={"result": "\n".join(lines), "variablesReference": 0})
        else:
            respond(req, success=False, message="peek failed")

    elif expr == "hello":
        if session:
            session.refresh()
            respond(req, body={"result": json.dumps(session.hello_data, indent=2),
                               "variablesReference": 0})
        else:
            respond(req, success=False, message="not attached")

    elif expr == "log":
        if session:
            session.refresh()
            lines = session.read_log_new()
            respond(req, body={"result": "\n".join(lines) if lines else "(no new log)",
                               "variablesReference": 0})
        else:
            respond(req, success=False, message="not attached")

    else:
        respond(req, success=False,
                message=f"unknown: {expr!r}. Try: peek 0xADDR [N], hello, log")


def handle_disconnect(session_box, req):
    s = session_box[0]
    if s:
        s.close()
        session_box[0] = None
    respond(req)
    event("terminated")


HANDLERS = {
    "initialize":        lambda sb, req: handle_initialize(sb[0], req),
    "attach":            lambda sb, req: handle_attach(sb, req),
    "launch":            lambda sb, req: handle_attach(sb, req),
    "configurationDone": lambda sb, req: handle_configuration_done(sb[0], req),
    "threads":           lambda sb, req: handle_threads(sb[0], req),
    "stackTrace":        lambda sb, req: handle_stack_trace(sb[0], req),
    "scopes":            lambda sb, req: handle_scopes(sb[0], req),
    "variables":         lambda sb, req: handle_variables(sb[0], req),
    "readMemory":        lambda sb, req: handle_read_memory(sb[0], req),
    "evaluate":          lambda sb, req: handle_evaluate(sb[0], req),
    "disconnect":        lambda sb, req: handle_disconnect(sb, req),
}


# ---------------------------------------------------------------------------
# Background log poller
# ---------------------------------------------------------------------------

def log_poller(session_box, stop_event):
    while not stop_event.is_set():
        s = session_box[0]
        if s and s.running:
            try:
                s.refresh()
                lines = s.read_log_new()
                for line in lines:
                    event("output", {"category": "console", "output": line + "\n"})
            except Exception:
                pass
        stop_event.wait(0.2)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

_cli_args = {}


def console_mode():
    """Human-readable interactive mode: shows the same data as DAP but as plain text."""
    mode = _cli_args.get("mode", "usb")
    if mode == "mock":
        s = MockSession()
    elif mode == "ram":
        s = RamSession(_cli_args["ram"])
    else:
        s = Session(_cli_args.get("port"))

    s.refresh()
    s.refresh_modules()
    h = s.hello_data

    print(f"=== fm1-dock connected ({mode}) ===")
    if mode == "ram":
        print(f"RAM dump: {_cli_args['ram']}")
    print()

    print("--- Crash Record ---")
    pc = h.get("crash_pc", 0)
    print(f"  crash_pc:     {'none' if not pc else f'{pc:#010x}'}")
    print(f"  crash_module: {'none' if not pc else h.get('crash_module', 0)}")
    print()

    print("--- Render Stats ---")
    worst = h.get("worst", 0)
    total = 8192
    pct = worst * 100.0 / total if total else 0
    print(f"  worst render: {pct:.1f}% ({worst} ticks)")
    print(f"  overruns:     {h.get('overruns', 0)}")
    print(f"  log_w:        {h.get('log_w', 0)}")
    print()

    print("--- Modules ---")
    if s.module_list:
        for m in s.module_list:
            print(f"  {m['name']} v{m.get('version',0)} {m.get('size',0)} KiB [{m.get('state','?')}]")
    else:
        print("  (none)")
    print()

    print("--- Boot Log ---")
    s.log_pos = max(0, h.get("log_w", 0) - 2048)
    lines = s.read_log_new()
    for line in lines:
        print(f"  {line}")
    if not lines:
        print("  (empty)")
    print()

    print(f"--- Hello (raw) ---")
    print(f"  {json.dumps(h, indent=2)}")
    print()

    if mode == "usb":
        print("Polling log (Ctrl+C to stop)...")
        try:
            while True:
                time.sleep(0.2)
                s.refresh()
                lines = s.read_log_new()
                for line in lines:
                    print(line)
        except KeyboardInterrupt:
            pass
    s.close()


def _find_emulator():
    """Locate fm1-live or fm1-emu binary."""
    import shutil
    env = os.environ.get("FM1_LIVE", "")
    if env and Path(env).is_file():
        return env
    home_bin = Path.home() / ".fm1-emulator" / "bin"
    for name in ("fm1-live", "fm1-emu"):
        p = home_bin / name
        if p.is_file():
            return str(p)
    for name in ("fm1-live", "fm1-emu"):
        found = shutil.which(name)
        if found:
            return found
    return None


def _boot_emulator(ms=2000):
    """Boot the emulator for `ms` milliseconds, return path to RAM dump."""
    import subprocess
    emu = _find_emulator()
    if not emu:
        sys.exit("fm1-live/fm1-emu not found. "
                 "Install: task -t .scratch/Taskfile.yml fm1-emu:download:fm1-emu")
    fwsc = SRC / "docs" / "fm1-dock-0.5.fwsc"
    if not fwsc.exists():
        sys.exit(f"Firmware not found: {fwsc}")
    ram_dir = SRC / "build" / "emu"
    ram_dir.mkdir(parents=True, exist_ok=True)
    ram = ram_dir / "ram.bin"
    print(f"[fm1dap] emulator: {emu}", file=sys.stderr)
    print(f"[fm1dap] firmware: {fwsc}", file=sys.stderr)
    print(f"[fm1dap] booting for {ms} ms...", file=sys.stderr)
    env = {**os.environ, "FM1_DUMPRAM": str(ram)}
    subprocess.run([emu, "--fwsc", str(fwsc), "--shot", str(ms)],
                   env=env, stdout=sys.stderr, stderr=sys.stderr, check=True)
    print(f"[fm1dap] RAM dump: {ram} ({ram.stat().st_size} bytes)", file=sys.stderr)
    return str(ram)


def main():
    global _cli_args
    ap = argparse.ArgumentParser(description="fm1-dock DAP debug adapter")
    ap.add_argument("--port", help="MIDI port name (default: auto-detect Felucca/FM1-DOCK/FM-1)")
    ap.add_argument("--ram", metavar="FILE", help="offline: read from emulator RAM dump instead of USB-MIDI")
    ap.add_argument("--emu", action="store_true", help="boot the emulator, then attach to its RAM dump")
    ap.add_argument("--mock", action="store_true", help="offline: use synthetic data (no hardware needed)")
    ap.add_argument("--console", action="store_true", help="human-readable output instead of DAP (for IDE Run configs)")
    args = ap.parse_args()

    if args.emu:
        ram_path = _boot_emulator()
        _cli_args = {"mode": "ram", "ram": ram_path}
    elif args.mock:
        _cli_args = {"mode": "mock"}
    elif args.ram:
        _cli_args = {"mode": "ram", "ram": args.ram}
    else:
        _cli_args = {"mode": "usb", "port": args.port}

    if args.console:
        console_mode()
        return

    session_box = [None]
    stop = threading.Event()
    poller = threading.Thread(target=log_poller, args=(session_box, stop), daemon=True)
    poller.start()

    try:
        while True:
            msg = read_message(sys.stdin)
            if msg is None:
                break
            cmd = msg.get("command", "")
            handler = HANDLERS.get(cmd)
            if handler:
                try:
                    handler(session_box, msg)
                except Exception as e:
                    respond(msg, success=False, message=str(e))
            else:
                respond(msg, success=True)
    except (KeyboardInterrupt, BrokenPipeError):
        pass
    finally:
        stop.set()
        if session_box[0]:
            session_box[0].close()


if __name__ == "__main__":
    main()
