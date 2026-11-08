#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""DAP debug adapter integration test.

Emulator mode (default): boots fm1-dock in the emulator, reads its state through
the DAP adapter, and verifies the boot log, crash record, render stats, and
memory reads are correct.

Live mode (--live): connects to a real FM-1 over USB-MIDI and runs the same DAP
request sequence. Requires python-rtmidi and an FM-1 plugged in.

  python3 tests/dap_test.py                # emulator
  python3 tests/dap_test.py --live         # live USB-MIDI
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "tools"))
FWSC_BUILD = SRC / "build" / "fm1-dock.fwsc"
FWSC_SHIPPED = SRC / "docs" / "fm1-dock-0.5.fwsc"
APP = SRC / "build" / "fm1-dock_xip.bin"
failures = []


def check(name, ok, detail=""):
    status = "ok  " if ok else "FAIL"
    print(f"  {status} {name}{': ' + detail if detail else ''}")
    if not ok:
        failures.append(name)


def find_fm1_live():
    """Locate the fm1-live emulator using the repo convention (dock.live())."""
    import dock
    try:
        return dock.live()
    except SystemExit:
        return None


# ---------------------------------------------------------------------------
# DAP transport helpers
# ---------------------------------------------------------------------------

def dap_session(extra_args, requests, ensure_ascii=True):
    """Run the DAP adapter with extra_args, send requests, return parsed responses/events."""
    parts = []
    for req in requests:
        body = json.dumps(req, ensure_ascii=ensure_ascii).encode("utf-8")
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
    (out_dir / "work").mkdir(exist_ok=True)
    # Prefer the tree's build over the shipped release package
    if FWSC_BUILD.exists():
        fw_arg = str(FWSC_BUILD)
    elif FWSC_SHIPPED.exists():
        fw_arg = str(FWSC_SHIPPED)
    else:
        fw_arg = None
    bin_arg = str(APP) if APP.exists() else None

    if fw_arg:
        cmd = [str(fm1_live), "--fwsc", fw_arg, "--shot", "2000"]
    elif bin_arg:
        cmd = [str(fm1_live), str(bin_arg), "--shot", "2000"]
    else:
        return None, f"no firmware: need {FWSC_BUILD} or {FWSC_SHIPPED} or {APP}"

    env = {**os.environ, "FM1_DUMPRAM": str(ram)}
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60, env=env,
                       cwd=str(out_dir))
    if not ram.exists():
        return None, f"no ram.bin produced (exit {r.returncode}): {r.stderr[:200]}"
    return ram, f"ram.bin {ram.stat().st_size} bytes"


# ---------------------------------------------------------------------------
# Shared DAP verification
# ---------------------------------------------------------------------------

def verify_dap(msgs, strict=True):
    """Verify DAP responses. strict=True expects a clean emulator boot (no crash,
    no overruns). strict=False accepts whatever the live device reports."""

    r = find_response(msgs, "initialize")
    check("dap_initialize", r and r["success"] and r["body"]["supportsReadMemoryRequest"])
    check("dap_initialized_event", find_event(msgs, "initialized") is not None)

    r = find_response(msgs, "attach")
    check("dap_attach", r and r["success"])

    ev = find_event(msgs, "stopped")
    check("dap_stopped_event", ev is not None,
          f"reason={ev['body']['reason']}" if ev else "missing")

    r = find_response(msgs, "threads")
    check("dap_threads", r and r["success"] and r["body"]["threads"][0]["name"] == "FM-1")

    r = find_response(msgs, "stackTrace")
    check("dap_stack", r and r["success"] and len(r["body"]["stackFrames"]) >= 1,
          r["body"]["stackFrames"][0]["name"] if r and r["success"] else "")

    r = find_response(msgs, "scopes")
    scope_names = [s["name"] for s in r["body"]["scopes"]] if r and r["success"] else []
    check("dap_scopes", "Crash Record" in scope_names and "Render Stats" in scope_names,
          ", ".join(scope_names))

    v = var_dict(msgs, 7)
    if v:
        if strict:
            check("crash_record", v.get("crash_pc") == "none" or v.get("crash_pc") == "0x00000000",
                  f"pc={v.get('crash_pc')} mod={v.get('crash_module')}")
        else:
            check("crash_record", "crash_pc" in v,
                  f"pc={v.get('crash_pc')} mod={v.get('crash_module')}")
    else:
        check("crash_record", False, "no crash variables")

    v = var_dict(msgs, 8)
    if v:
        worst = v.get("worst_render_%", "?")
        overruns = v.get("overruns", "?")
        check("render_stats", worst != "?" and overruns != "?",
              f"worst={worst}% overruns={overruns}")
        if strict:
            check("no_overruns", overruns == "0",
                  f"overruns={overruns} (expected 0 for a clean 2 s boot)")
    else:
        check("render_stats", False, "no stats variables")
        if strict:
            check("no_overruns", False)

    v = var_dict(msgs, 9)
    if v:
        names = list(v.keys())
        check("module_list", len(names) >= 1,
              ", ".join(n.strip().lstrip("> ") for n in names))
    else:
        check("module_list", False, "no module variables")

    r = find_response(msgs, "evaluate", seq=10)
    if r and r["success"]:
        data = json.loads(r["body"]["result"])
        check("hello_json", "worst" in data and "log_w" in data,
              f"worst={data.get('worst')} log_w={data.get('log_w')}")
    else:
        check("hello_json", False, r.get("message", "no response") if r else "no response")

    r = find_response(msgs, "evaluate", seq=11)
    if r and r["success"]:
        result = r["body"]["result"]
        check("peek_memory", "0x01c00000:" in result, result.split("\n")[0])
    else:
        check("peek_memory", False, r.get("message", "no response") if r else "no response")

    # The boot log is emitted as output events during attach. After that,
    # evaluate "log" returns only new entries (correctly "(no new log)" if
    # nothing happened since attach).
    r = find_response(msgs, "evaluate", seq=12)
    check("eval_log_response", r is not None and r.get("success"),
          r.get("message", "no response") if r else "no response")
    # Check that the boot log was emitted in output events during attach
    boot_outputs = [m for m in msgs
                    if m.get("type") == "event" and m.get("event") == "output"
                    and m.get("body", {}).get("category") in ("stdout", "console")]
    boot_text = " ".join(m["body"]["output"] for m in boot_outputs)
    if strict:
        has_boot = "boot" in boot_text.lower() or "started" in boot_text.lower()
        check("boot_log", has_boot, boot_text[:80] if boot_text else "(no output events)")
    else:
        check("boot_log", len(boot_outputs) > 0, boot_text[:80] if boot_text else "(empty)")

    r = find_response(msgs, "disconnect")
    check("dap_disconnect", r and r["success"])
    check("terminated_event", find_event(msgs, "terminated") is not None)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_mock_non_ascii_framing():
    """Non-ASCII bytes in a request body must not desync the DAP framing.
    Regression: text-mode stdin counts characters, not bytes, so a multi-byte
    UTF-8 character shifts Content-Length and the adapter dies with JSONDecodeError."""
    print("mock: non-ASCII framing...")
    requests = [
        {"seq": 1, "type": "request", "command": "initialize",
         "arguments": {"clientID": "Félucca", "adapterID": "fm1"}},
        {"seq": 2, "type": "request", "command": "threads", "arguments": {}},
        {"seq": 3, "type": "request", "command": "disconnect", "arguments": {}},
    ]
    # ensure_ascii=False sends the literal UTF-8 bytes (é = 2 bytes) so
    # Content-Length (bytes) differs from the character count. This is what
    # VS Code does: it does not escape non-ASCII to \uXXXX.
    msgs = dap_session(["--mock"], requests, ensure_ascii=False)
    r_init = find_response(msgs, "initialize")
    check("nonascii_initialize", r_init is not None and r_init.get("success"),
          f"got {len(msgs)} msgs" if r_init is None else "")
    r_threads = find_response(msgs, "threads")
    check("nonascii_threads", r_threads is not None and r_threads.get("success"),
          "missing" if r_threads is None else "")
    r_disc = find_response(msgs, "disconnect")
    check("nonascii_disconnect", r_disc is not None and r_disc.get("success"),
          "missing" if r_disc is None else "")


def test_mock_unknown_command():
    """Unknown DAP commands must return success:false, not success:true."""
    print("mock: unknown command...")
    requests = [
        {"seq": 1, "type": "request", "command": "initialize",
         "arguments": {"clientID": "test", "adapterID": "fm1"}},
        {"seq": 2, "type": "request", "command": "bogusCommand", "arguments": {}},
        {"seq": 3, "type": "request", "command": "disconnect", "arguments": {}},
    ]
    msgs = dap_session(["--mock"], requests)
    r = find_response(msgs, "bogusCommand")
    check("unknown_cmd_fails", r is not None and r.get("success") is False,
          f"success={r.get('success')}" if r else "no response")
    check("unknown_cmd_message", r is not None and r.get("message", "") != "",
          r.get("message", "") if r else "")


def test_mock_direct_read_memory():
    """readMemory must work directly (not just through evaluate peek)."""
    print("mock: direct readMemory...")
    requests = [
        {"seq": 1, "type": "request", "command": "initialize",
         "arguments": {"clientID": "test", "adapterID": "fm1"}},
        {"seq": 2, "type": "request", "command": "attach", "arguments": {}},
        {"seq": 3, "type": "request", "command": "readMemory",
         "arguments": {"memoryReference": "0x1C00000", "count": 16}},
        {"seq": 4, "type": "request", "command": "disconnect", "arguments": {}},
    ]
    msgs = dap_session(["--mock"], requests)
    r = find_response(msgs, "readMemory")
    check("readmem_success", r is not None and r.get("success"),
          r.get("message", "") if r else "no response")
    if r and r.get("success"):
        import base64
        data = base64.b64decode(r["body"]["data"])
        check("readmem_has_data", len(data) == 16, f"got {len(data)} bytes")
    else:
        check("readmem_has_data", False, "no data")


def test_mock_no_private_paths():
    """The adapter must not reference private emulator dirs or scratch taskfiles.
    The repo convention is FM1_LIVE / dock.live()."""
    print("mock: no private paths...")
    # Compose the forbidden strings so this test doesn't match itself
    emu_dir = ".fm1-" + "emulator"
    scratch_tf = ".scratch/" + "Taskfile"
    adapter = (SRC / "tools" / "fm1dap.py").read_text()
    check("no_fm1_emulator_dir", emu_dir not in adapter,
          "tools/fm1dap.py still references private emulator dir")
    check("no_scratch_taskfile", scratch_tf not in adapter,
          "tools/fm1dap.py still references scratch taskfile")


def test_mock_peek_address_bounds():
    """readMemory and evaluate peek must reject addresses outside RAM/XIP ranges.
    This prevents faulting the FM-1 with SFR accesses or unmapped addresses."""
    print("mock: peek address bounds...")
    # An SFR address that would fault real hardware
    sfr_addr = "0x1EEE240"
    requests = [
        {"seq": 1, "type": "request", "command": "initialize",
         "arguments": {"clientID": "test", "adapterID": "fm1"}},
        {"seq": 2, "type": "request", "command": "attach", "arguments": {}},
        # RAM address: should succeed
        {"seq": 3, "type": "request", "command": "readMemory",
         "arguments": {"memoryReference": "0x1C00000", "count": 16}},
        # SFR address: should fail
        {"seq": 4, "type": "request", "command": "readMemory",
         "arguments": {"memoryReference": sfr_addr, "count": 4}},
        # Evaluate peek at SFR: should fail
        {"seq": 5, "type": "request", "command": "evaluate",
         "arguments": {"expression": f"peek {sfr_addr} 1"}},
        {"seq": 6, "type": "request", "command": "disconnect", "arguments": {}},
    ]
    msgs = dap_session(["--mock"], requests)
    # RAM read should work
    r = find_response(msgs, "readMemory", seq=3)
    check("peek_ram_ok", r is not None and r.get("success"),
          r.get("message", "") if r else "no response")
    # SFR read should be rejected
    r = find_response(msgs, "readMemory", seq=4)
    check("peek_sfr_rejected", r is not None and r.get("success") is False,
          f"success={r.get('success')}" if r else "no response")
    # Evaluate peek at SFR should be rejected
    r = find_response(msgs, "evaluate", seq=5)
    check("eval_peek_sfr_rejected", r is not None and r.get("success") is False,
          f"success={r.get('success')}" if r else "no response")


def test_mock_no_duplicate_boot_log():
    """The boot log must not be emitted twice: once by attach and again by the poller.
    After attach emits the boot log, log_pos should advance past it."""
    print("mock: no duplicate boot log...")
    requests = [
        {"seq": 1, "type": "request", "command": "initialize",
         "arguments": {"clientID": "test", "adapterID": "fm1"}},
        {"seq": 2, "type": "request", "command": "attach", "arguments": {}},
        {"seq": 3, "type": "request", "command": "configurationDone", "arguments": {}},
        # Give the poller a cycle to run
        {"seq": 4, "type": "request", "command": "threads", "arguments": {}},
        {"seq": 5, "type": "request", "command": "evaluate",
         "arguments": {"expression": "log"}},
        {"seq": 6, "type": "request", "command": "disconnect", "arguments": {}},
    ]
    msgs = dap_session(["--mock"], requests)
    # Count how many output events contain "boot" (the first mock log line is "0 boot")
    boot_outputs = [m for m in msgs
                    if m.get("type") == "event" and m.get("event") == "output"
                    and "boot" in m.get("body", {}).get("output", "").lower()
                    and m["body"].get("category") == "stdout"]
    check("boot_log_once", len(boot_outputs) <= 1,
          f"'boot' appeared in {len(boot_outputs)} output events (expected at most 1)")
    # The evaluate log should return "(no new log)" since all lines were already emitted
    r = find_response(msgs, "evaluate", seq=5)
    if r and r.get("success"):
        check("log_after_boot_empty", "no new log" in r["body"]["result"],
              r["body"]["result"][:60])
    else:
        check("log_after_boot_empty", False, "no response")


def test_ide_files_not_in_docs():
    """IDE configs and extension source must not be under docs/ (published to Pages)."""
    print("source: IDE files not in docs/...")
    bad = list((SRC / "docs" / "ide").rglob("*")) if (SRC / "docs" / "ide").exists() else []
    check("no_docs_ide", len(bad) == 0,
          f"docs/ide/ still exists with {len(bad)} entries")
    # The files should be under tools/ide/ instead
    check("tools_ide_exists", (SRC / "tools" / "ide").is_dir(),
          "tools/ide/ does not exist")


def test_emu_prefers_tree_build():
    """--emu must prefer build/fm1-dock.fwsc over the shipped docs/fm1-dock-0.5.fwsc.
    The adapter source should reference build/ as the default, not hard-code the 0.5 release."""
    print("source: emu prefers tree build...")
    adapter = (SRC / "tools" / "fm1dap.py").read_text()
    # The boot function should look for build/fm1-dock.fwsc before the docs package
    check("emu_build_first", "build" in adapter and "fm1-dock.fwsc" in adapter,
          "adapter does not reference build/fm1-dock.fwsc")
    # The hard-coded 0.5 path should only be a fallback, not the primary
    lines = adapter.splitlines()
    build_line = next((i for i, l in enumerate(lines) if "build" in l and "fm1-dock.fwsc" in l), 999)
    docs_line = next((i for i, l in enumerate(lines) if "docs" in l and "fm1-dock-0.5" in l), 999)
    check("emu_build_before_docs", build_line < docs_line,
          f"build ref at line {build_line+1}, docs ref at line {docs_line+1}")


def test_licensing_metadata():
    """VS Code extension must have SPDX header, license and repository in package.json,
    and a LICENSING.md row for the DAP adapter contribution."""
    print("source: licensing metadata...")
    ext_js = (SRC / "tools" / "ide" / ".vscode" / "extensions" / "fm1-dock-debug" / "extension.js").read_text()
    check("spdx_extension_js", "SPDX-License-Identifier" in ext_js,
          "extension.js missing SPDX header")

    pkg = json.loads((SRC / "tools" / "ide" / ".vscode" / "extensions" / "fm1-dock-debug" / "package.json").read_text())
    check("pkg_license", pkg.get("license") == "GPL-3.0-only",
          f"license={pkg.get('license')}")
    check("pkg_repository", "repository" in pkg,
          "package.json missing repository field")

    licensing = (SRC / "LICENSING.md").read_text()
    check("licensing_dap_row", "fm1dap" in licensing or "DAP" in licensing,
          "LICENSING.md has no row for the DAP adapter")


def test_mock_console_render_pct():
    """console_mode must use hello['block'] for render %, not 8192.
    Mock hello has worst=3800, block=139319, so worst % = 3800/139319*100 = 2.7%.
    Using the old 8192 divisor would give 46.4%.
    Module sizes are in bytes and should be printed as bytes or real KiB."""
    print("mock: console render %...")
    cmd = [sys.executable, str(SRC / "tools" / "fm1dap.py"), "--mock", "--console"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
    out = r.stdout
    # Find "worst render:" line and check it uses the right divisor
    import re
    m = re.search(r"worst render:\s+([\d.]+)%", out)
    check("console_render_pct", m is not None, "no worst render line found")
    if m:
        pct = float(m.group(1))
        # With block=139319 and worst=3800: 3800/139319*100 = 2.7%
        # With old 8192 divisor: 3800/8192*100 = 46.4%
        check("console_render_pct_value", pct < 10.0,
              f"got {pct}% (expected ~2.7%, not ~46.4%)")
    # Check module sizes don't have KiB label for byte values
    # Mock modules: sine=2048, CS-1=32768, OberVave=28672 (all bytes)
    kib_lines = [l for l in out.splitlines() if "KiB" in l]
    # If KiB is printed, the values should be real KiB (divided by 1024), not raw bytes
    for l in kib_lines:
        m2 = re.search(r"(\d+)\s*KiB", l)
        if m2:
            val = int(m2.group(1))
            check("console_module_size_unit", val <= 64,
                  f"got {val} KiB in '{l.strip()}' (looks like raw bytes, not KiB)")


def test_dap_over_emulator():
    fm1_live = find_fm1_live()
    if fm1_live is None:
        print("no fm1-live found (set FM1_LIVE or build fm1-emu: cargo build --release)")
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

        print("running DAP adapter with --ram...")
        msgs = dap_session(["--ram", str(ram)], REQUESTS)
        verify_dap(msgs, strict=True)


def test_dap_over_live():
    print("connecting to FM-1 over USB-MIDI...")
    msgs = dap_session([], REQUESTS)
    if not msgs:
        check("live_connect", False, "no DAP responses (is FM-1 plugged in? is python-rtmidi installed?)")
        return
    r = find_response(msgs, "attach")
    if not r or not r.get("success"):
        msg = r.get("message", "unknown error") if r else "no attach response"
        check("live_connect", False, msg)
        return
    check("live_connect", True)
    verify_dap(msgs, strict=False)


def main():
    live = "--live" in sys.argv

    # Mock-mode tests always run (no hardware or emulator needed)
    test_mock_non_ascii_framing()
    test_mock_unknown_command()
    test_mock_direct_read_memory()
    test_mock_peek_address_bounds()
    test_mock_no_duplicate_boot_log()
    test_ide_files_not_in_docs()
    test_mock_no_private_paths()
    test_emu_prefers_tree_build()
    test_mock_console_render_pct()
    test_licensing_metadata()

    if live:
        test_dap_over_live()
    else:
        test_dap_over_emulator()
    print()
    if failures:
        print(f"FAILED: {', '.join(failures)}")
        return 1
    print("all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
