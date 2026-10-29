#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Scintilla (joakimeriksson/scintilla-fm1) into this module: its engine (engine/) and its app
(firmware/firmware/src/scintilla.c, as app/scintilla.c), both as they are.
  modules/scintilla/take.py ~/work/synthex-fm1
Built with -DHB_MODULE (module.cfg) the app takes its platform from sc_harbor.h (fm1-dock's: the host's
calls, as CS-1's), leaves the DIN jack to the host (its MIDI comes through midi()) and runs as
app_init() once and app_loop() a pass; the module's ui() fills its key_vel / key_press from the host's
key events (sc_module.c). This checks that the app still has those, and fails if it does not."""
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
src = Path(sys.argv[1])
app = (src / "firmware/firmware/src/scintilla.c").read_text()
for need in ('#ifdef HB_MODULE', '#include "sc_harbor.h"', "static void app_init(void)", "static void app_loop(void)",
             "static void app_audio(int32_t *out, uint32_t n)", "static void redraw_all(void)",
             "static uint8_t key_vel[27], key_press[27]", "static void note_on(", "static void note_off("):
    if need not in app:
        raise SystemExit(f"take.py: scintilla.c has no {need!r}: the module needs it (sc_module.c)")
(HERE / "engine").mkdir(exist_ok=True)
for f in (src / "engine").glob("sc_*.[ch]"):
    shutil.copy(f, HERE / "engine" / f.name)
(HERE / "app").mkdir(exist_ok=True)
(HERE / "app" / "scintilla.c").write_text(app)
print("modules/scintilla: engine/ and app/scintilla.c taken from", src)
