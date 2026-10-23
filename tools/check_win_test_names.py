#!/usr/bin/env python3
"""Static check: the names of what the Windows native tests create have ONE source, ci/win-test-names.psd1.

The native tests, the CI steps around them and the station script must agree on what they create and on the product
names they act on; a name spelt twice drifts. So:

  * the windows job's FAT32 step reads the names from the .psd1, and tests/win/integration/CMakeLists.txt generates
    the tests' WinTestNames.h from it;
  * a test-side name (TEST_SIDE below) written out as a literal anywhere else, in code rather than a comment,
    in the files the tests, the CI or the station script are made of, fails;
  * the product names the tests act on (the SCM service name, the firewall group, the install folder leaf, the
    Apps & features key's leaf),
    which the product keeps in its own source, must equal the .psd1's;
  * qualification/ws7-station.ps1 runs alone on the station PC and keeps its names as literals: they must
    equal the .psd1's.

Run from anywhere: python3 tools/check_win_test_names.py [--root <tree>]. Exit 0 = consistent.
"""
import argparse
import re
import sys
from pathlib import Path

KEYS = ("ServiceName", "InstallFolderLeaf", "FirewallGroup", "UninstallKeyLeaf", "AccountPrefix", "ScratchPrefix",
        "ReportLog", "KeywordMatrixCiGroup", "KeywordMatrixTestGroup", "FatLabel", "FatImagePrefix", "AsideInfix",
        "StationEvidenceLeaf")
# Names only tests and CI give: never written out outside the .psd1 (and the generated header).
TEST_SIDE = ("AccountPrefix", "ScratchPrefix", "ReportLog", "KeywordMatrixCiGroup", "KeywordMatrixTestGroup",
             "FatLabel", "FatImagePrefix")
# The files that read the .psd1, and the text that shows they do.
READERS = {
    ".github/workflows/ci.yml": "Import-PowerShellDataFile ci/win-test-names.psd1",
    "tests/win/integration/CMakeLists.txt": '"${CMAKE_SOURCE_DIR}/ci/win-test-names.psd1"',
}
# Where test-side literals are looked for (globs under the root).
SCANNED = ("tests/win/**/*.cpp", "tests/win/**/*.h", "tests/win/**/CMakeLists.txt", "ci/scripts/win-*.ps1",
           ".github/workflows/ci.yml", "qualification/*.ps1")

errors = []


def err(msg):
    errors.append(msg)


def read_names(path):
    names = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        m = re.match(r"\s*([A-Za-z]+)\s*=\s*'([^']*)'", line)
        if m:
            if m.group(1) in names:
                err(f"{path.name}: {m.group(1)} is defined twice")
            names[m.group(1)] = m.group(2)
    for k in KEYS:
        if not names.get(k):
            err(f"{path.name}: no single-quoted value for {k}")
    for k in names:
        if k not in KEYS:
            err(f"{path.name}: {k} is not a known name (add it to tools/check_win_test_names.py)")
    return names


def strip_comments(text, kind):
    """The code of a file, without its comments (string literals are kept)."""
    if kind == "cpp":
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        return "\n".join(re.sub(r"(^|\s)//.*$", r"\1", l) for l in text.splitlines())
    if kind == "ps1":
        text = re.sub(r"<#.*?#>", "", text, flags=re.S)
    # ps1, yml, cmake: a '#' that starts a line or follows whitespace begins a comment.
    return "\n".join(re.sub(r"(^|\s)#.*$", r"\1", l) for l in text.splitlines())


def kind_of(path):
    if path.suffix in (".cpp", ".h"):
        return "cpp"
    if path.suffix == ".ps1":
        return "ps1"
    return "hash"


def check(root):
    src = root / "ci" / "win-test-names.psd1"
    if not src.is_file():
        err("ci/win-test-names.psd1 is missing")
        return
    n = read_names(src)
    if errors:
        return

    for rel, needle in READERS.items():
        p = root / rel
        if not p.is_file() or needle not in p.read_text(encoding="utf-8"):
            err(f"{rel}: does not read the names from ci/win-test-names.psd1 (expected: {needle})")

    for pattern in SCANNED:
        for p in sorted(root.glob(pattern)):
            code = strip_comments(p.read_text(encoding="utf-8"), kind_of(p))
            for k in TEST_SIDE:
                if n[k] in code:
                    line = next(i + 1 for i, l in enumerate(code.splitlines()) if n[k] in l)
                    err(f"{p.relative_to(root)}:{line}: the {k} name '{n[k]}' is written out; read it from "
                        f"ci/win-test-names.psd1 (WinTestNames.h in the native tests)")

    def constant(rel, regex, key):
        p = root / rel
        m = re.search(regex, p.read_text(encoding="utf-8")) if p.is_file() else None
        if not m:
            err(f"{rel}: the {key} constant was not found")
        elif m.group(1) != n[key]:
            err(f"{rel}: {key} is '{m.group(1)}', ci/win-test-names.psd1 says '{n[key]}'")

    constant("src/core/ServicePlan.h", r'kServiceName\[\]\s*=\s*"([^"]*)"', "ServiceName")
    constant("src/core/FirewallPlan.h", r'kGroup\[\]\s*=\s*"([^"]*)"', "FirewallGroup")
    constant("src/platform/win/ProcessGuard.cpp", r'return root \+ QStringLiteral\("([^"]*)"\)', "InstallFolderLeaf")
    constant("src/core/AppLocations.cpp", r'base \+ QStringLiteral\("/([^"]*)"\)', "InstallFolderLeaf")
    # The C++ literal escapes its backslashes ("...\\Uninstall\\<leaf>"); only the leaf is compared.
    constant("src/platform/win/UninstallEntry.h", r'kKey\[\]\s*=\s*"[^"]*\\\\Uninstall\\\\([^"\\]*)"', "UninstallKeyLeaf")
    station = "qualification/ws7-station.ps1"
    constant(station, r"\$ServiceName = '([^']*)'", "ServiceName")
    constant(station, r'"\$LockDir(\.[^$"]*)\$stamp"', "AsideInfix")
    constant(station, r'"\$DataDir(\.[^$"]*)\$stamp"', "AsideInfix")
    constant(station, r'\[string\]\$Evidence = "\$env:SystemDrive\\([^"]*)"', "StationEvidenceLeaf")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    check(Path(ap.parse_args().root))
    if errors:
        print("check_win_test_names: FAILED")
        for e in errors:
            print("  - " + e)
        return 1
    print("check_win_test_names: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
