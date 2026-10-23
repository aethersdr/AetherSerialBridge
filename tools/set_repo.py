#!/usr/bin/env python3
"""The repository location is one value: ASB_REPO in the top-level CMakeLists.txt.

The project lives at github.com/<ASB_REPO>. The code takes it from the CMake cache variable (the systemd unit's
Documentation= link); the user-facing docs and packaging texts spell it out. This tool keeps the two together.

  set_repo.py <owner/name>   Rewrite ASB_REPO's default and every occurrence of the current value in the user-facing
                             files (below) to <owner/name>. Running it again with the same value changes nothing.
  set_repo.py --check        Exit 1 if a user-facing file names this project at any repository but ASB_REPO, or the
                             code writes this project's GitHub link out instead of taking it from ASB_REPO.

"This project" is any owner/<name> whose name is ASB_REPO's name or aether-serial-bridge (a move that also renames
the repository keeps catching the old links). Links to other projects (AetherSDR, ser2net, Qt, FTXUI...) are fine.

User-facing files: README.md, CHANGELOG.md, docs/**/*.md and the *.txt / *.md files under packaging/. Not scanned:
vendor/ (upstream files). --check also reads src/**: a GitHub link in the code must come from ASB_REPO, never be
written out.

Usage: set_repo.py [--root <repository root>] (<owner/name> | --check)   (exit 0 done or clean, 1 findings, 2 usage)
"""
import argparse
import re
import sys
from pathlib import Path

CMAKE_SET = re.compile(r'^set\(ASB_REPO "([^"]*)" CACHE STRING', re.M)
# The same shape the CMakeLists.txt accepts.
VALID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9-]*/[A-Za-z0-9._-]+$")
# An owner/name pair, as a GitHub link (github.com/owner/name) or on its own (`gh ... -R owner/name`). On its own it
# may not follow '/', '.', '-' or a word character, so file paths such as /etc/aether-serial-bridge are not read as one.
REPO = re.compile(r"(?:github\.com/|(?<![\w/.\-]))([A-Za-z0-9][A-Za-z0-9-]*)/([A-Za-z0-9._-]+)")
PROJECT_NAME = "aether-serial-bridge"


def repo_value(root):
    text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    found = CMAKE_SET.findall(text)
    if len(found) != 1:
        raise SystemExit(f"set_repo: expected one 'set(ASB_REPO \"...\" CACHE STRING' in CMakeLists.txt, found {len(found)}")
    return found[0]


def user_facing(root):
    files = [root / "README.md", root / "CHANGELOG.md"]
    files.extend(sorted((root / "docs").rglob("*.md")))
    for pattern in ("*.txt", "*.md"):
        files.extend(sorted((root / "packaging").rglob(pattern)))
    return [p for p in files if p.is_file()]


def code_files(root):
    return [p for p in sorted((root / "src").rglob("*")) if p.suffix in {".cpp", ".h", ".rc", ".in"} and p.is_file()]


def clean(name):
    name = name.rstrip(".")
    return name[:-4] if name.endswith(".git") else name


def findings(root):
    repo = repo_value(root)
    out = []
    if not VALID.match(repo):
        out.append(f"CMakeLists.txt: ASB_REPO '{repo}' is not owner/name")
    names = {repo.split("/")[-1].lower(), PROJECT_NAME}
    for p in user_facing(root) + code_files(root):
        in_code = p.suffix not in {".md", ".txt"}
        rel = p.relative_to(root)
        for n, line in enumerate(p.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            for m in REPO.finditer(line):
                owner, name = m.group(1), clean(m.group(2))
                if name.lower() not in names:
                    continue
                link = m.group(0).startswith("github.com/")
                if in_code:
                    if link:
                        out.append(f"{rel}:{n}: github.com/{owner}/{name} is written out; the code takes it from ASB_REPO")
                elif f"{owner}/{name}" != repo:
                    out.append(f"{rel}:{n}: {owner}/{name} is not ASB_REPO ({repo})")
    return out


def rewrite(root, new):
    old = repo_value(root)
    changed = []
    cm = root / "CMakeLists.txt"
    text = cm.read_text(encoding="utf-8")
    updated = CMAKE_SET.sub(lambda m: m.group(0).replace(f'"{old}"', f'"{new}"'), text)
    if updated != text:
        cm.write_text(updated, encoding="utf-8")
        changed.append(("CMakeLists.txt", 1))
    if old != new:
        for p in user_facing(root):
            t = p.read_text(encoding="utf-8")
            count = t.count(old)
            if count:
                p.write_text(t.replace(old, new), encoding="utf-8")
                changed.append((str(p.relative_to(root)), count))
    return old, changed


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=Path(__file__).resolve().parent.parent, type=Path)
    ap.add_argument("--check", action="store_true", help="report files that name another repository (exit 1)")
    ap.add_argument("repo", nargs="?", help="the new location, owner/name")
    a = ap.parse_args(argv)
    root = a.root.resolve()
    if a.check == bool(a.repo):
        ap.print_usage(sys.stderr)
        print("set_repo: give either <owner/name> or --check", file=sys.stderr)
        return 2
    if a.check:
        found = findings(root)
        for f in found:
            print(f)
        repo = repo_value(root)
        print(f"set_repo --check: {len(found)} finding(s); ASB_REPO is {repo}, "
              f"{len(user_facing(root))} user-facing files and {len(code_files(root))} code files read")
        return 1 if found else 0
    if not VALID.match(a.repo):
        print(f"set_repo: '{a.repo}' is not owner/name", file=sys.stderr)
        return 2
    old, changed = rewrite(root, a.repo)
    for path, count in changed:
        print(f"{path}: {count} replaced")
    print(f"set_repo: {old} -> {a.repo}; {len(changed)} file(s) changed")
    left = findings(root)
    for f in left:
        print(f)
    return 1 if left else 0


if __name__ == "__main__":
    sys.exit(main())
