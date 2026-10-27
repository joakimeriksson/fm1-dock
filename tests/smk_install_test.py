#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/fm1_install.py's rules for the two boards, on the packages build/ has (./build.sh, ./build.sh --board smk37):
  tests/smk_install_test.py
  - each package's board is read from the package itself (its region table), not from an identity;
  - an FM-1 package is refused on anything that reports an SMK-37 (running, or in update mode), an SMK-37 package
    on anything else, before anything is sent;
  - the SMK-37 models are one keyboard (the Starrykey 37 Play takes the SMK-37 Pro's), never with the FM-1;
  - only Felucca's own update loaders (ota-FM-1_9NN, ota-SMK-37 Pro_9NN, ...) are resumed."""
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "tools"))
import fm1_install as I  # noqa: E402

failures = []


def check(case, ok, why):
    print(f"  {'ok  ' if ok else 'FAIL'} {case}: {why}")
    if not ok:
        failures.append(case)


def main():
    pkgs = {"fm1": SRC / "build" / "fm1-dock.fwsc", "smk": SRC / "build" / "fm1-dock-smk37.fwsc"}
    for board, path in pkgs.items():
        if not path.exists():
            print(f"  (no {path.name}: its cases are skipped)")
            continue
        product, image = I.load_package(str(path), False)
        check(f"{board} board", I.package_board(image) == board, f"{path.name} ({product}) reads as {I.package_board(image)}")
        for dev in ("FM-1_921", "SMK-37 Pro_016", "ota-SMK-37 Elite_900", "STARRYKEY-37 PLAY_016", "MKE-P37_016"):
            smk = bool(I.SMK_MODEL_RE.fullmatch(I.model_of(dev)))
            refused = I.board_refusal(I.model_of(dev), image) is not None
            check(f"{board} on {dev}", refused == (smk != (board == "smk")),
                  ("refused" if refused else "taken") + (", the same keyboard" if I.same_keyboard(dev, product) else ""))
    check("same keyboard", I.same_keyboard("STARRYKEY-37 PLAY_016", "SMK-37 Pro_921") and
          I.same_keyboard("ota-SMK-37 Elite_900", "MKE-P37_921") and not I.same_keyboard("FM-1_921", "SMK-37 Pro_921"),
          "the SMK-37 models are one keyboard, never an FM-1")
    check("own loaders", all(I.OWN_LOADER_RE.fullmatch(x) for x in ("ota-FM-1_900", "ota-SMK-37 Pro_900",
                                                                      "ota-SMK-37 Elite_921", "ota-MKE-P37_900")) and
          not any(I.OWN_LOADER_RE.fullmatch(x) for x in ("ota-FM-1_015", "ota-SMK-37 Pro_016")),
          "Felucca's own update loaders are resumed, M-Vave's are not")
    check("ports", all(I.PORT_RE.search(x) for x in ("Felucca", "SMK-37 Pro", "MKE-P37", "STARRYKEY-37 PLAY")) and
          not I.PORT_RE.search("IAC Driver Bus 1"), "the SMK-37's ports are probed, other gear is not")
    print("install rules " + ("FAILED: " + ", ".join(failures) if failures else "passed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
