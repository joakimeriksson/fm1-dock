#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""modules/orrery/orrery/ from ORRERY's own tree (orrery-fm1, Joakim Eriksson): the module (module/), the portable
session under it (core/), the instruments' adapters and the effects box (instruments/), the effects (effects/), and
the engines that are only ORRERY's (engines/: the ENSEMBLE pads, and its own state of Scintilla's Synthex), as they
are there. Its tools, its tests and its auditions stay in that tree.

Three of its engines are modules of this tree, and are not copied: the ARP 2600 (modules/arp2600), the Eminent 310
(modules/eminent) and the Mini Pops 7 (modules/minipops). ORRERY is built from those very files, so the module
and the three synths cannot drift apart; the includes that name them as "engines/..." in ORRERY's tree name them
bare here, and module.cfg (written by this script from ORRERY's own) points at them. ORRERY's tree copies the same
three from here with its own take.py: this script stops if its copies are not this tree's files.

Run it again on a new ORRERY:
    modules/orrery/take.py ~/work/orrery-fm1"""
import re
import shutil
import sys
from pathlib import Path

src = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.home() / "work/orrery-fm1"
here = Path(__file__).resolve().parent
out = here / "orrery"
# the engines that live here: {file: its module's directory}
OURS = {"arp_engine.c": "arp2600", "arp_engine.h": "arp2600", "arp_presets.h": "arp2600",
        "em_engine.c": "eminent", "em_engine.h": "eminent",
        "mp_engine.c": "minipops", "mp_engine.h": "minipops", "mp_tables.h": "minipops"}
LEFT = {"hb_echo.c"}                                # (the tape echo effects/echo.c was ported from: not built)

for name, mod in OURS.items():
    if (src / "engines" / name).read_bytes() != (here.parent / mod / name).read_bytes():
        raise SystemExit(f"take.py: {src}/engines/{name} is not modules/{mod}/{name}: bring the two together first "
                         f"(ORRERY's take.py copies from here)")
if out.exists():
    shutil.rmtree(out)
for d in ["module", "core", "instruments", "effects", "engines"]:
    (out / d).mkdir(parents=True)
    for f in sorted((src / d).iterdir()):
        if f.is_file() and f.suffix in (".c", ".h") and not (d == "engines" and (f.name in OURS or f.name in LEFT)):
            shutil.copy(f, out / d / f.name)
(out / "LICENSES").mkdir()
for f in ["LICENSING.md", "LICENSES/MIT-Felucca-Engines.txt"]:
    shutil.copy(src / f, out / f)

# "engines/arp_engine.h" -> "arp_engine.h": found in its module here (module.cfg's includes)
inc = re.compile(r'#include "engines/(' + "|".join(re.escape(n) for n in OURS if n.endswith(".h")) + r')"')
n = 0
for f in sorted(out.rglob("*.[ch]")):
    t = f.read_text()
    t2, k = inc.subn(r'#include "\1"   /* fm1-dock: the engine of this tree (take.py) */', t)
    if k:
        f.write_text(t2)
        n += k
assert n, "no include of an engine of this tree: has ORRERY's layout changed?"

# module.cfg from ORRERY's: its paths from modules/orrery/, the three engines from their modules
cfg = []
for line in (src / "module/module.cfg").read_text().splitlines():
    if line.startswith("#"):
        continue
    key, _, val = line.partition("=")

    def path(p):
        if not p.startswith("../"):
            return p
        name = p.rsplit("/", 1)[1]
        return f"../{OURS[name]}/{name}" if p.startswith("../engines/") and name in OURS else "orrery/" + p[3:]

    if key.strip() == "sources":
        own = [f"orrery/module/{f.name}" for f in sorted((src / "module").glob("*.c"))]
        val = " " + " ".join(own + [path(p) for p in val.split()])
    elif key.strip() == "includes":                 # (as -I from this tree's root: the build's container sees that)
        continue
    elif key.strip() == "cflags":
        val += " " + " ".join(f"-Imodules/{d}" for d in ["orrery/orrery", "orrery/orrery/module", "orrery/orrery/engines", *sorted(set(OURS.values()))])
    elif len(key.split()) > 1 and key.split()[0] == "cflags":
        key = "cflags " + path(key.split(None, 1)[1].strip()) + " "
    cfg.append(f"{key}={val}" if _ else line)
head = ["# ORRERY: the seventies' synths of a Jarre record in one module (orrery-fm1; written by take.py from its",
        "# module.cfg: the ARP 2600, the Eminent and the Mini Pops are this tree's own modules' engines)"]
(here / "module.cfg").write_text("\n".join(head + cfg) + "\n")
print(f"orrery/: taken from {src} ({sum(1 for _ in out.rglob('*.[ch]'))} files, {n} includes pointed at this tree's engines)")
