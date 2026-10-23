#!/usr/bin/env bash
# The glibc-tree gate's control (v1.1 INV-121): a copy of the extracted AppImage with one planted Qt plugin that needs
# GLIBC_2.38 must FAIL `release_audit.py glibc-tree` (the gate must reach usr/plugins and see the requirement).
#
# usage: tools/glibc-tree-control.sh <squashfs-root>
#
# The plant is this container's own libstdc++ (a real ELF file with a real version-needs section) with its highest
# GLIBC_2.NN requirement renamed to GLIBC_2.38 (same length): no download and no second container image.
set -euo pipefail
root="${1:?squashfs-root}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cp -a "$root" "$tmp/tree"
lib="/usr/lib/$(uname -m)-linux-gnu/libstdc++.so.6"
mkdir -p "$tmp/tree/usr/plugins/platforms"
python3 - "$lib" "$tmp/tree/usr/plugins/platforms/libasb-glibc-control.so" <<'PY'
import re
import sys
data = open(sys.argv[1], "rb").read()
found = sorted(set(re.findall(rb"GLIBC_2\.[0-9]{2}\0", data)), key=lambda v: int(v[8:10]))
if not found:
    sys.exit("no GLIBC_2.NN requirement in %s to rename" % sys.argv[1])
open(sys.argv[2], "wb").write(data.replace(found[-1], b"GLIBC_2.38\0"))
print("planted %s with %s renamed to GLIBC_2.38" % (sys.argv[2], found[-1][:-1].decode()))
PY
python3 "$here/release_audit.py" glibc-tree-control "$tmp/tree" 2.35
