#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""DAP debug adapter integration test: boots fm1-dock in the emulator, reads its
state through the DAP adapter, and verifies the boot log, crash record, render
stats, and memory reads are correct.

Requires: fm1-live (from fm1-emu) and a firmware package (docs/fm1-dock-0.5.fwsc)
or a built firmware (build/fm1-dock_xip.bin).

  FM1_LIVE=~/.fm1-emulator/bin/fm1-live python3 tests/dap_test.py
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
FWSC = SRC / "docs" / "fm1-dock-0.5.fwsc"
APP = SRC / "build" / "fm1-dock_xip.bin"
failures = []


def check(name, ok, detail=""):
    status = "ok  " if ok else "FAIL"
    print(f"  {status} {name}{': ' + detail if detail else ''}")
    if not ok:
        failures.append(name)


def find_fm1_live():
    env = os.environ.get("FM1_LIVE", "")
    if env:
        p = Path(env)
        if p.exists():
            return p
    for candidate in [
        Path.home() / ".fm1-emulator" / "bin" / "fm1-live",
        Path.home() / ".fm1-emulator" / "bin" / "fm1-emu",
        Path.home() / "work" / "fm1-emu" / "target" / "release" / "fm1-live",
    ]:
        if candidate.exists():
            return candidate
    return None


# ---------------------------------------------------------------------------
# DAP transport helpers
# ---------------------------------------------------------------------------

def dap_session(extra_args, requests):
    """Run the DAP adapter with extra_args, send requests, return parsed responses/events."""
    parts = []
    for req in requests:
        body = json.dumps(req).encode("utf-8")
        parts.append(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body)

    cmd = [sys.executable, str(SRC / "tools" / "fm1dap.py")] + extra_args
    r = subprocess.run(cmd, input=b"".join(parts), capture_output=True, timeout=10)
    if r.returncode:
        print(f"    adapter stderr: {r.stderr.decode('utf-8', errors='replace')[:300]}", file=sys.stderr)

    out = []
    data = r.stdout
    i = 0
    while i < len(data):
        if not data[i:i + 15].startswith(b"Content-Length:"):
            break
        sep = data.index(b"\r\n\r\n", i)
        length = int(data[i:sep].split(b":")[1].strip())
        body = data[sep + 4:sep + 4 + length]
        out.append(json.loads(body))
        i = sep + 4 + length
    return out


def find_response(msgs, command, seq=None):
    for m in msgs:
        if m.get("type") == "response" and m.get("command") == command:
            if seq is None or m.get("request_seq") == seq:
                return m
    return None


def find_event(msgs, name):
    for m in msgs:
        if m.get("type") == "event" and m.get("event") == name:
            return m
    return None


def var_dict(msgs, seq):
    r = find_response(msgs, "variables", seq=seq)
    if not r or not r.get("success"):
        return None
    return {x["name"]: x["value"] for x in r["body"]["variables"]}


REQUESTS = [
    {"seq": 1, "type": "request", "command": "initialize",
     "arguments": {"clientID": "dap_test", "adapterID": "fm1"}},
    {"seq": 2, "type": "request", "command": "attach", "arguments": {}},
    {"seq": 3, "type": "request", "command": "configurationDone", "arguments": {}},
    {"seq": 4, "type": "request", "command": "threads", "arguments": {}},
    {"seq": 5, "type": "request", "command": "stackTrace",
     "arguments": {"threadId": 1}},
    {"seq": 6, "type": "request", "command": "scopes",
     "arguments": {"frameId": 1}},
    {"seq": 7, "type": "request", "command": "variables",
     "arguments": {"variablesReference": 1000}},
    {"seq": 8, "type": "request", "command": "variables",
     "arguments": {"variablesReference": 2000}},
    {"seq": 9, "type": "request", "command": "variables",
     "arguments": {"variablesReference": 3000}},
    {"seq": 10, "type": "request", "command": "evaluate",
     "arguments": {"expression": "hello"}},
    {"seq": 11, "type": "request", "command": "evaluate",
     "arguments": {"expression": "peek 0x1C00000 4"}},
    {"seq": 12, "type": "request", "command": "evaluate",
     "arguments": {"expression": "log"}},
    {"seq": 13, "type": "request", "command": "disconnect", "arguments": {}},
]


# ---------------------------------------------------------------------------
# Emulator: boot fm1-dock, produce a RAM dump
# ---------------------------------------------------------------------------

def boot_emulator(fm1_live, out_dir):
    """Run fm1-dock in fm1-live, return (ram_path, ok) or (None, error_msg)."""
    ram = out_dir / "ram.bin"
    fw_arg = str(FWSC) if FWSC.exists() else None
    bin_arg = str(APP) if APP.exists() else None

    if fw_arg:
        cmd = [str(fm1_live), "--fwsc", fw_arg, "--shot", "2000"]
    elif bin_arg:
        cmd = [str(fm1_live), str(bin_arg), "--shot", "2000"]
    else:
        return None, f"no firmware: need {FWSC} or {APP}"

    env = {**os.environ, "FM1_DUMPRAM": str(ram)}
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60, env=env)
    if not ram.exists():
        return None, f"no ram.bin produced (exit {r.returncode}): {r.stderr[:200]}"
    return ram, f"ram.bin {ram.stat().st_size} bytes"


# ---------------------------------------------------------------------------
# The test
# ---------------------------------------------------------------------------

def test_dap_over_emulator():
    fm1_live = find_fm1_live()
    if fm1_live is None:
        print("no fm1-live found (set FM1_LIVE or install via .scratch/Taskfile.yml)")
        return 1

    print(f"using fm1-live: {fm1_live}")
    print("booting fm1-dock in emulator...")
    with tempfile.TemporaryDirectory() as td:
        out = Path(td)
        ram, detail = boot_emulator(fm1_live, out)
        if ram is None:
            check("emu_boot", False, detail)
            return
        check("emu_boot", True, detail)

        # --- run the DAP adapter against the RAM dump ---
        print("running DAP adapter with --ram...")
        msgs = dap_session(["--ram", str(ram)], REQUESTS)

        # DAP lifecycle
        r = find_response(msgs, "initialize")
        check("dap_initialize", r and r["success"] and r["body"]["supportsReadMemoryRequest"])
        check("dap_initialized_event", find_event(msgs, "initialized") is not None)

        r = find_response(msgs, "attach")
        check("dap_attach", r and r["success"])

        ev = find_event(msgs, "stopped")
        check("dap_stopped_event", ev is not None,
              f"reason={ev['body']['reason']}" if ev else "missing")

        # threads
        r = find_response(msgs, "threads")
        check("dap_threads", r and r["success"] and r["body"]["threads"][0]["name"] == "FM-1")

        # stack trace
        r = find_response(msgs, "stackTrace")
        check("dap_stack", r and r["success"] and len(r["body"]["stackFrames"]) >= 1,
              r["body"]["stackFrames"][0]["name"] if r and r["success"] else "")

        # scopes
        r = find_response(msgs, "scopes")
        scope_names = [s["name"] for s in r["body"]["scopes"]] if r and r["success"] else []
        check("dap_scopes", "Crash Record" in scope_names and "Render Stats" in scope_names,
              ", ".join(scope_names))

        # crash record from real firmware boot (should be clean: no crash)
        v = var_dict(msgs, 7)
        if v:
            check("crash_record", v.get("crash_pc") == "none" or v.get("crash_pc") == "0x00000000",
                  f"pc={v.get('crash_pc')} mod={v.get('crash_module')}")
        else:
            check("crash_record", False, "no crash variables")

        # render stats from real firmware run
        v = var_dict(msgs, 8)
        if v:
            worst = v.get("worst_render_%", "?")
            overruns = v.get("overruns", "?")
            check("render_stats", worst != "?" and overruns != "?",
                  f"worst={worst}% overruns={overruns}")
            check("no_overruns", overruns == "0",
                  f"overruns={overruns} (expected 0 for a clean 2 s boot)")
        else:
            check("render_stats", False, "no stats variables")
            check("no_overruns", False)

        # module list
        v = var_dict(msgs, 9)
        if v:
            names = list(v.keys())
            check("module_list", len(names) >= 1,
                  ", ".join(n.strip().lstrip("> ") for n in names))
        else:
            check("module_list", False, "no module variables")

        # evaluate hello: real firmware state as JSON
        r = find_response(msgs, "evaluate", seq=10)
        if r and r["success"]:
            data = json.loads(r["body"]["result"])
            check("hello_json", "worst" in data and "log_w" in data,
                  f"worst={data.get('worst')} log_w={data.get('log_w')}")
        else:
            check("hello_json", False, r.get("message", "no response") if r else "no response")

        # evaluate peek: read bytes from RAM base
        r = find_response(msgs, "evaluate", seq=11)
        if r and r["success"]:
            result = r["body"]["result"]
            check("peek_memory", "0x01c00000:" in result, result.split("\n")[0])
        else:
            check("peek_memory", False, r.get("message", "no response") if r else "no response")

        # evaluate log: the boot log from the firmware's ring buffer
        r = find_response(msgs, "evaluate", seq=12)
        if r and r["success"]:
            result = r["body"]["result"]
            has_boot = "boot" in result.lower() or "started" in result.lower()
            check("boot_log", has_boot,
                  result.splitlines()[0] if result.strip() and result != "(no new log)" else result)
        else:
            check("boot_log", False, r.get("message", "no response") if r else "no response")

        # disconnect
        r = find_response(msgs, "disconnect")
        check("dap_disconnect", r and r["success"])
        check("terminated_event", find_event(msgs, "terminated") is not None)


def main():
    test_dap_over_emulator()
    print()
    if failures:
        print(f"FAILED: {', '.join(failures)}")
        return 1
    print("all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
