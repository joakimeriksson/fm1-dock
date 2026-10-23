#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Scintilla's app and engine into this module, as they are in its repository (github.com/joakimeriksson/
# scintilla-fm1): firmware/firmware/src/scintilla.c and engine/. Nothing here is edited by hand: change them
# there (where tests/run.sh and the firmware's tests check them), then take them again.
#   modules/scintilla/take.sh [SCINTILLA_DIR]       (default: ~/work/synthex-fm1)
set -e
cd "$(dirname "$0")"
S="${1:-$HOME/work/synthex-fm1}"
rm -rf scintilla
mkdir scintilla
cp "$S"/engine/sc_*.c "$S"/engine/sc_*.h "$S"/firmware/firmware/src/scintilla.c scintilla/
echo "$(git -C "$S" rev-parse --short HEAD)$(git -C "$S" diff --quiet HEAD -- engine firmware/firmware/src/scintilla.c || echo +)" > scintilla/FROM
echo "took $(ls scintilla | wc -l | tr -d ' ') files from $S at $(cat scintilla/FROM)"
