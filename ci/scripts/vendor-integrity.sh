#!/usr/bin/env bash
# vendor-integrity (master §2.2, WS-2): the vendored AetherSDR tree is exactly upstream + patches.
#
# usage: ci/scripts/vendor-integrity.sh [--self-test]
#
# Reads the pinned commit from vendor/aethersdr/PROVENANCE.md, fetches the listed files fresh from
# AetherSDR (AETHERSDR_REPO overrides the source, as for tools/sync-aethersdr.sh), and re-runs
# tools/sync-aethersdr.sh on a scratch copy of the committed tree: that re-applies the patches to
# the fresh upstream bytes, re-runs gen_theme_seed.py --check, regenerates PROVENANCE.md (every
# upstream and vendored SHA-256) and refuses unlisted files. The result must equal the committed
# tree byte for byte.
#
# --self-test also proves the check can fail: on scratch copies it flips one byte in an unpatched
# file, flips one byte in a patched file, adds an unlisted file and breaks a patch's context, and
# requires the matching failure for each (a fetch failure never counts as the expected failure).
#
# Exit codes: 0 verified; 1 usage or environment; 2 upstream could not be read; 3 a patch or the
# seed check failed; 4 unlisted file; 5 the committed tree differs from upstream + patches;
# 6 a self-test control did not fail the way it must.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
root="$PWD"
vendor="$root/vendor/aethersdr"
sync="$root/tools/sync-aethersdr.sh"

self_test=0
case "${1:-}" in
    "") ;;
    --self-test) self_test=1 ;;
    *) echo "usage: $0 [--self-test]" >&2; exit 1 ;;
esac

[ -f "$vendor/FILES" ] && [ -f "$vendor/PROVENANCE.md" ] || {
    echo "vendor-integrity: vendor/aethersdr/FILES or PROVENANCE.md is missing" >&2; exit 1; }
sha="$(sed -n 's/^- Upstream commit: `\([0-9a-f]\{40\}\)`$/\1/p' "$vendor/PROVENANCE.md")"
[[ "$sha" =~ ^[0-9a-f]{40}$ ]] || {
    echo "vendor-integrity: no 'Upstream commit' line in PROVENANCE.md" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# ---- one fetch of the listed upstream files, shared by every run below -------------------------
source_repo="${AETHERSDR_REPO:-https://github.com/aethersdr/AetherSDR.git}"
if [ -d "$source_repo" ] && git -C "$source_repo" cat-file -e "$sha^{commit}" 2>/dev/null; then
    upstream="$source_repo"
else
    upstream="$work/upstream"
    mapfile -t files < <(grep -v -e '^#' -e '^[[:space:]]*$' "$vendor/FILES")
    git init -q "$upstream"
    git -C "$upstream" remote add origin "$source_repo"
    if ! git -C "$upstream" fetch -q --depth 1 --filter=blob:none origin "$sha" \
        || ! git -C "$upstream" sparse-checkout set --no-cone -- "${files[@]/#//}" \
        || ! git -C "$upstream" -c advice.detachedHead=false checkout -q "$sha"; then
        echo "vendor-integrity: could not fetch $sha from $source_repo" >&2
        exit 2
    fi
fi

# check_tree <tree>: re-derive <tree> from upstream + its own FILES and patches; 0 when identical.
check_tree() {
    local tree="$1" scratch rc=0
    scratch="$(mktemp -d "$work/check.XXXXXX")"
    cp -a "$tree/." "$scratch/"
    AETHERSDR_REPO="$upstream" "$sync" "$sha" --dest "$scratch" >"$scratch.log" 2>&1 || rc=$?
    if [ "$rc" -ne 0 ]; then
        cat "$scratch.log" >&2
        return "$rc"
    fi
    if ! diff -r -q "$tree" "$scratch" >"$scratch.diff" 2>&1; then
        echo "vendor-integrity: MISMATCH, the tree is not upstream $sha + patches:" >&2
        cat "$scratch.diff" >&2
        return 5
    fi
    return 0
}

if [ "$self_test" -eq 0 ]; then
    rc=0
    check_tree "$vendor" || rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "vendor-integrity: FAILED (exit $rc)" >&2
        exit "$rc"
    fi
    count="$(grep -c -v -e '^#' -e '^[[:space:]]*$' "$vendor/FILES")"
    echo "vendor-integrity: OK, $count files = AetherSDR $sha + $(ls "$vendor/patches" | wc -l) patch(es)"
    exit 0
fi

# ---- self-test: each control must fail, with its own failure --------------------------------
expect() {
    local name="$1" want="$2" tree="$3" rc=0
    check_tree "$tree" 2>"$work/$name.err" || rc=$?
    if [ "$rc" -eq "$want" ]; then
        echo "vendor-integrity self-test: $name -> exit $rc, as required"
    else
        echo "vendor-integrity self-test: $name -> exit $rc, required $want" >&2
        cat "$work/$name.err" >&2
        exit 6
    fi
}
control() {
    local d="$work/control-$1"
    mkdir -p "$d"
    cp -a "$vendor/." "$d/"
    echo "$d"
}
flip_byte() {  # flips the low bit of the byte at offset $2 of file $1
    python3 - "$1" "$2" <<'PY'
import sys
path, off = sys.argv[1], int(sys.argv[2])
data = bytearray(open(path, "rb").read())
data[off] ^= 0x01
open(path, "wb").write(bytes(data))
PY
}

expect clean 0 "$(control clean)"

d="$(control unpatched-byte)"; flip_byte "$d/src/gui/Theme.h" 1000
expect unpatched-byte 5 "$d"

d="$(control patched-byte)"; flip_byte "$d/src/core/ThemeManager.cpp" 20000
expect patched-byte 5 "$d"

d="$(control binary-byte)"; flip_byte "$d/third_party/inter/Inter-Bold.ttf" 4096
expect binary-byte 5 "$d"

d="$(control provenance)"; sed -i 's/^| `LICENSE` | `\(.\)/| `LICENSE` | `0/' "$d/PROVENANCE.md"
cmp -s "$vendor/PROVENANCE.md" "$d/PROVENANCE.md" && { echo "self-test: provenance edit did nothing" >&2; exit 6; }
expect provenance 5 "$d"

d="$(control unlisted)"; echo "not upstream" > "$d/src/gui/Extra.h"
expect unlisted 4 "$d"

d="$(control broken-patch)"
sed -i '0,/^ #include "ThemeManager.h"$/s// #include "ThemeManagerX.h"/' "$d/patches/0001-user-theme-dir-hook.patch"
cmp -s "$vendor/patches/0001-user-theme-dir-hook.patch" "$d/patches/0001-user-theme-dir-hook.patch" \
    && { echo "self-test: patch edit did nothing" >&2; exit 6; }
expect broken-patch 3 "$d"

echo "vendor-integrity self-test: every control failed as required"
