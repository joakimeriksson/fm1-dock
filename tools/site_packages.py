#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The packages the page installs (docs/install/fm1install.js): each repository's latest release, its .fwsc
copied into the site, with fw/index.json. A page cannot download a release asset from GitHub itself (no CORS),
so the site keeps a copy. Each copy is checked against the SHA-256 GitHub gives for the release asset, and as
tools/fm1_install.py checks a package before it installs one (identity, update loader, every file's CRC).

  tools/site_packages.py OUT_DIR ID=OWNER/REPO ... [--modules DIR]      (GH_TOKEN, or gh logged in)
  e.g. tools/site_packages.py _site/fw fm1-dock=joakimeriksson/fm1-dock fm1-sid=joakimeriksson/fm1-sid --modules docs/modules

A release may carry the SMK-37's packages beside the FM-1's (fm1-dock: NAME-X.Y-smk37.fwsc, -smk37-elite, -smk37-mke):
those go into the firmware's "variants", by the model their identity names, for the page to pick by the keyboard.

The modules the page adds to an fm1-dock (FM-1 SID, AMB-1, ...): a .dock attached to that firmware's latest release when
there is one, else DIR's copy (DIR/modules.json says what it is; the page's catalog reads the rest of it). Each is checked
as tools/hbdeploy.py checks one (its header). An entry "packaged" (in fm1-dock's package) has no file; an entry whose
.dock is not in DIR yet is left out of index.json, and the page shows it without its button.
"""
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fm1_install  # noqa: E402
import hbmod  # noqa: E402


def gh_json(path):
    return json.loads(subprocess.run(["gh", "api", path], check=True, capture_output=True, text=True).stdout)


def module_header(raw, name):
    """a .dock's header, checked as tools/hbmod.py reads one (magic, relocations, CRC), or SystemExit"""
    try:
        h, relocs, image = hbmod.parse(raw)
    except Exception as e:
        raise SystemExit(f"{name}: not a good fm1-dock module ({e})")
    if hbmod.crc_of(raw[:76], relocs, image) != h["crc"]:
        raise SystemExit(f"{name}: the module's CRC does not match")
    return h


def modules(out, index, mdir, releases):
    """the modules (mdir/modules.json) into out, preferring a .dock on that firmware's latest release"""
    index["modules"] = {}
    for fid, m in json.loads((mdir / "modules.json").read_text()).items():
        if m.get("packaged"):                   # in fm1-dock's package: nothing to add, the page only shows it
            continue
        rel = releases.get(fid)
        docks = [a for a in rel["assets"] if a["name"].endswith(".dock")] if rel else []
        if len(docks) != 1 and not (mdir / m["file"]).exists():
            print(f"{fid:10} {m['file']:28} not here yet: the page shows it without its button")
            continue
        if len(docks) == 1:
            a = docks[0]
            subprocess.run(["gh", "release", "download", rel["tag_name"], "-R", rel["repo"], "-p", a["name"], "-D",
                            str(out), "--clobber"], check=True)
            path, version, source = out / a["name"], rel["tag_name"].removeprefix("v"), rel["html_url"]
            raw = path.read_bytes()
            if a.get("digest") and a["digest"] != "sha256:" + hashlib.sha256(raw).hexdigest():
                raise SystemExit(f"{a['name']}: SHA-256 differs from GitHub's")
        else:
            path = out / m["file"]
            path.write_bytes((mdir / m["file"]).read_bytes())
            version, source, raw = m["version"], m["source"], path.read_bytes()
        h = module_header(raw, path.name)
        index["modules"][fid] = {"name": h["name"], "version": version, "file": path.name,
                                 "sha256": hashlib.sha256(raw).hexdigest(), "size": len(raw), "abi": h["abi"],
                                 "source": source, "songs": m.get("songs", ""), "title": m.get("title", h["name"]),
                                 "by": m.get("by", "")}
        print(f"{fid:10} {path.name:28} module, abi {h['abi']}  {index['modules'][fid]['sha256'][:16]}")


def main(argv):
    mdir = None
    if "--modules" in argv:
        i = argv.index("--modules")
        mdir = Path(argv[i + 1])
        argv = argv[:i] + argv[i + 2:]
    if len(argv) < 2:
        raise SystemExit(__doc__)
    out = Path(argv[0])
    out.mkdir(parents=True, exist_ok=True)
    index = {"updated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "firmwares": {}}
    releases = {}
    for arg in argv[1:]:
        fid, _, repo = arg.partition("=")
        rel = gh_json(f"repos/{repo}/releases/latest")
        releases[fid] = {**rel, "repo": repo}
        assets = [a for a in rel["assets"] if a["name"].endswith(".fwsc")]
        fm1 = [a for a in assets if "-smk37" not in a["name"]]
        if len(fm1) != 1:
            raise SystemExit(f"{repo} {rel['tag_name']}: {len(fm1)} FM-1 .fwsc assets, expected one")
        variants = {}
        for a in fm1 + [a for a in assets if "-smk37" in a["name"]]:
            subprocess.run(["gh", "release", "download", rel["tag_name"], "-R", repo, "-p", a["name"], "-D", str(out),
                            "--clobber"], check=True)
            path = out / a["name"]
            raw = path.read_bytes()
            sha = hashlib.sha256(raw).hexdigest()
            if a.get("digest") and a["digest"] != "sha256:" + sha:
                raise SystemExit(f"{repo} {a['name']}: SHA-256 {sha}, GitHub says {a['digest']}")
            try:
                product, image = fm1_install.load_package(str(path), False)
            except fm1_install.InstallError as e:
                raise SystemExit(f"{repo} {a['name']}: {e}")
            board = fm1_install.package_board(image)
            if (board == "smk") != ("-smk37" in a["name"]):
                raise SystemExit(f"{repo} {a['name']}: laid out for {board}, named for the other board")
            print(f"{fid:10} {a['name']:28} {product:14} {sha[:16]}")
            if board == "smk":
                variants[fm1_install.model_of(product)] = {"file": a["name"], "sha256": sha, "size": len(raw), "product": product}
                continue
            index["firmwares"][fid] = {"name": rel["name"] or a["name"], "version": rel["tag_name"].removeprefix("v"),
                                       "tag": rel["tag_name"], "file": a["name"], "sha256": sha, "size": len(raw),
                                       "product": product, "release": rel["html_url"]}
        if variants:
            index["firmwares"][fid]["variants"] = variants
    if mdir:
        modules(out, index, mdir, releases)
    (out / "index.json").write_text(json.dumps(index, indent=1) + "\n")


if __name__ == "__main__":
    main(sys.argv[1:])
