#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Which modules are behind the firmware they come from, and can they be taken again by themselves.

  tools/modules_check.py                  a table: each module, its source, the version it was taken from, the newest
  tools/modules_check.py --try b3 fimba   take those again from their source's newest release (their take.py) and
                                          build them (tools/hbmod.py); a module that fails is put back as it was
  tools/modules_check.py --try-all        the same for every module that is behind

A module made from another firmware says where it came from on module.cfg's first line ("# B3 ORGAN: R-Jeb/setBfree-fm1
(v0.2, 7014954), ..."): its repository, and a version and / or a commit. The newest is the highest version tag of that
repository (git ls-remote, no account needed), else its default branch's head. A module is behind when the newest tag is
a higher version, or (only a commit known) when the head has moved on. --try runs the module's take.py on a checkout of
the newest and builds it: "taken" (it built: review `git diff modules/<dir>`), or where it stopped (an edit of take.py
that no longer matches the new source, a build error), which a person then fixes in take.py.
"""
import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MODULES = ROOT / "modules"
REPO = re.compile(r"\b([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)\b")
VER = re.compile(r"\bv?(\d+(?:\.\d+){1,3})\b")
SHA = re.compile(r"\b([0-9a-f]{7,40})\b")


def vkey(v):
    return tuple(int(x) for x in re.findall(r"\d+", v))


def git(*args, cwd=None):
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, timeout=300)


def source(mod):
    """(repo, version or None, commit or None) from module.cfg's first line, or None (a module of fm1-dock's own)"""
    cfg = mod / "module.cfg"
    if not cfg.exists() or not ((mod / "take.py").exists() or (mod / "take.mjs").exists()):
        return None
    line = cfg.read_text().splitlines()[0]
    m = REPO.search(line.split(":", 1)[1] if ":" in line else line)
    if not m or m.group(1).count(".") > 2 and "/" not in m.group(1):
        return None
    repo = m.group(1)
    if repo.endswith((".c", ".h", ".py", ".mjs")) or "/" not in repo:
        return None
    rest = line[m.end():]
    v = VER.search(rest)
    sha = SHA.search(rest)
    return repo, (v.group(1) if v else None), (sha.group(1) if sha else None)


def newest(repo):
    """(tag or None, version or None, commit) of the repository's newest release, else of its head"""
    url = f"https://github.com/{repo}"
    tags = git("ls-remote", "--tags", "--refs", url)
    best = None
    for l in tags.stdout.splitlines():
        sha, ref = l.split("\t")
        tag = ref.rsplit("/", 1)[-1]
        m = VER.fullmatch(tag) or VER.search(tag)
        if m and (best is None or vkey(m.group(1)) > vkey(best[1])):
            best = (tag, m.group(1), sha)
    if best:
        return best
    head = git("ls-remote", url, "HEAD").stdout.split()
    return (None, None, head[0] if head else None)


def status(mod):
    src = source(mod)
    if not src:
        return None
    repo, have_v, have_sha = src
    tag, new_v, new_sha = newest(repo)
    if new_v and have_v:
        behind = vkey(new_v) > vkey(have_v)
    elif new_sha and have_sha:
        behind = not new_sha.startswith(have_sha)
    else:
        behind = None
    return {"dir": mod.name, "repo": repo, "have": have_v or have_sha or "?", "newest": new_v or (new_sha or "?")[:7],
            "tag": tag, "sha": new_sha, "behind": behind}


def try_take(st):
    mod = MODULES / st["dir"]
    take = mod / "take.py"
    if not take.exists():
        return "no take.py (take.mjs: by hand)"
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "src"
        args = ["clone", "-q", "--depth", "1", "--recurse-submodules", "--shallow-submodules"]
        if st["tag"]:
            args += ["--branch", st["tag"]]
        if git(*args, f"https://github.com/{st['repo']}", str(src)).returncode:
            return "its source couldn't be fetched"
        p = subprocess.run([sys.executable, str(take), str(src)], cwd=ROOT, capture_output=True, text=True)
        if p.returncode:
            err = [l for l in (p.stderr or p.stdout).strip().splitlines() if l.strip()]
            put_back(mod)
            return "take.py stopped: " + (err[-1][:200] if err else f"exit {p.returncode}")
        b = subprocess.run([sys.executable, "tools/hbmod.py", f"modules/{st['dir']}"], cwd=ROOT, capture_output=True, text=True)
        if b.returncode:
            err = [l for l in (b.stderr + b.stdout).splitlines() if "error" in l.lower()][:3]
            put_back(mod)
            return "taken, but it doesn't build: " + (" | ".join(err)[:300] or f"exit {b.returncode}")
    return "taken and built: review git diff modules/" + st["dir"]


def put_back(mod):
    git("checkout", "--", str(mod.relative_to(ROOT)), cwd=ROOT)
    git("clean", "-qfd", "--", str(mod.relative_to(ROOT)), cwd=ROOT)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--try", dest="try_", nargs="+", metavar="DIR", help="take these again from their newest")
    ap.add_argument("--try-all", action="store_true", help="take every module that is behind again")
    a = ap.parse_args()
    rows = [s for s in (status(m) for m in sorted(p for p in MODULES.iterdir() if p.is_dir())) if s]
    print(f"{'module':<12} {'source':<34} {'taken from':<12} {'newest':<12} behind")
    for s in rows:
        b = {True: "YES", False: "no", None: "?"}[s["behind"]]
        print(f"{s['dir']:<12} {s['repo']:<34} {s['have']:<12} {s['newest']:<12} {b}")
    want = set(a.try_ or [])
    if a.try_all:
        want |= {s["dir"] for s in rows if s["behind"]}
    for s in rows:
        if s["dir"] in want:
            print(f"\n{s['dir']}: {try_take(s)}", flush=True)


if __name__ == "__main__":
    main()
