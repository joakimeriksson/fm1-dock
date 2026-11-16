#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The ENSEMBLE engine from ORRERY's tree (github.com/joakimeriksson/orrery-fm1), where it lives: ens_engine.c,
ens_engine.h and ens_tables.h as they are there (its tables' generator, tools/gen_ens_tables.py, stays there),
with the licence of the DSP blocks it uses. The module around it (ens_module.c) is this tree's.
    modules/ensemble/take.py ~/work/orrery-fm1"""
import shutil
import sys
from pathlib import Path

src = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.home() / "work/orrery-fm1"
here = Path(__file__).resolve().parent
for f in ["ens_engine.c", "ens_engine.h", "ens_tables.h"]:
    shutil.copy(src / "engines" / f, here / f)
shutil.copy(src / "LICENSES/MIT-Felucca-Engines.txt", here / "LICENSE-MIT-Felucca-Engines.txt")
print("ensemble/: the engine taken from", src)
