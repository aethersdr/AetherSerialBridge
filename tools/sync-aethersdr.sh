#!/usr/bin/env bash
# Vendor the AetherSDR closure at a pinned commit (master §2.2, WS-2).
#
# usage: tools/sync-aethersdr.sh <commit-sha> [--dest <dir>]
#
#   <commit-sha>   full 40-hex AetherSDR commit (a fetch by abbreviated SHA does not work)
#   --dest <dir>   the vendored tree to write (default: vendor/aethersdr of this checkout). It must
#                  already hold FILES and patches/; ci/scripts/vendor-integrity.sh points it at a
#                  scratch copy of the committed tree.
#
# Environment:
#   AETHERSDR_REPO  where the upstream bytes come from: a local AetherSDR clone that has the
#                   commit, or a git URL (default https://github.com/aethersdr/AetherSDR.git,
#                   fetched as a shallow, blob-less partial clone into a temporary directory).
#
# Steps, in order (any failure leaves --dest untouched):
#   1. read every path in FILES from the commit (upstream bytes);
#   2. `git apply --check`, then apply, patches/*.patch in name order, in a staging directory
#      outside any git repository; a patch may touch only listed files, must add a GPL-3.0 §5(a)
#      notice ("modified for Aether Serial Bridge") to each of them, and every file it touches
#      must end up different from upstream;
#   3. run the vendored tools/gen_theme_seed.py --check against the staged tree;
#   4. write PROVENANCE.md (upstream and vendored SHA-256 per file, patch SHA-256s);
#   5. fail if --dest holds any file that is neither listed in FILES nor FILES, PROVENANCE.md or
#      patches/*.patch; otherwise copy the staged files and PROVENANCE.md into --dest.
#
# Exit codes: 0 done; 1 usage or environment; 2 upstream could not be read; 3 a patch does not
# apply, touches an unlisted file or changes nothing, or the theme seed check fails; 4 unlisted
# files in --dest.
set -euo pipefail

die() { local code="$1"; shift; echo "sync-aethersdr: $*" >&2; exit "$code"; }

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sha=""
dest="$here/vendor/aethersdr"
while [ $# -gt 0 ]; do
    case "$1" in
        --dest) dest="${2:?--dest needs a directory}"; shift 2 ;;
        -h|--help) sed -n '2,32p' "${BASH_SOURCE[0]}"; exit 0 ;;
        -*) die 1 "unknown option $1" ;;
        *) [ -z "$sha" ] || die 1 "more than one commit given"; sha="$1"; shift ;;
    esac
done
[[ "$sha" =~ ^[0-9a-f]{40}$ ]] || die 1 "need the full 40-hex commit SHA, got '${sha}'"
[ -f "$dest/FILES" ] || die 1 "$dest/FILES is missing"
[ -d "$dest/patches" ] || die 1 "$dest/patches/ is missing"
dest="$(cd "$dest" && pwd)"
command -v python3 >/dev/null || die 1 "python3 is required (gen_theme_seed.py --check)"
command -v sha256sum >/dev/null || die 1 "sha256sum is required"

upstream_url="https://github.com/aethersdr/AetherSDR"
notice_marker="modified for Aether Serial Bridge"
repo="${AETHERSDR_REPO:-$upstream_url.git}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
# git apply inside a repository can silently skip paths outside the current directory; the staging
# directory must not be inside one.
if git -C "$work" rev-parse --git-dir >/dev/null 2>&1; then
    die 1 "the temporary directory $work is inside a git repository; set TMPDIR elsewhere"
fi

# ---- the list ---------------------------------------------------------------------------------
mapfile -t files < <(grep -v -e '^#' -e '^[[:space:]]*$' "$dest/FILES")
[ "${#files[@]}" -gt 0 ] || die 1 "FILES lists nothing"
if [ "$(printf '%s\n' "${files[@]}" | LC_ALL=C sort -u)" != "$(printf '%s\n' "${files[@]}")" ]; then
    die 1 "FILES must be sorted (LC_ALL=C) and free of duplicates"
fi
for f in "${files[@]}"; do
    case "$f" in
        /*|*..*|FILES|PROVENANCE.md|patches/*) die 1 "FILES entry not allowed: $f" ;;
    esac
done

# ---- 1. upstream bytes ------------------------------------------------------------------------
if [ -d "$repo" ] && git -C "$repo" cat-file -e "$sha^{commit}" 2>/dev/null; then
    src_git=(git -C "$repo")
else
    # Shallow and blob-less, then a sparse checkout of exactly the listed paths: one batched
    # blob fetch instead of one round trip per file.
    mirror="$work/upstream"
    git init -q "$mirror"
    git -C "$mirror" remote add origin "$repo"
    git -C "$mirror" fetch -q --depth 1 --filter=blob:none origin "$sha" \
        || die 2 "could not fetch $sha from $repo"
    git -C "$mirror" sparse-checkout set --no-cone -- "${files[@]/#//}" \
        || die 2 "sparse-checkout of the listed paths failed"
    git -C "$mirror" -c advice.detachedHead=false checkout -q "$sha" \
        || die 2 "could not check out $sha"
    src_git=(git -C "$mirror")
fi
mkdir -p "$work/raw" "$work/stage"
for f in "${files[@]}"; do
    mkdir -p "$work/raw/$(dirname "$f")" "$work/stage/$(dirname "$f")"
    "${src_git[@]}" show "$sha:$f" > "$work/raw/$f" \
        || die 2 "$f is not in AetherSDR at $sha"
    cp "$work/raw/$f" "$work/stage/$f"
done

# Is $1 one of FILES? A loop, not `printf | grep -q`: under pipefail grep's early exit made printf die of SIGPIPE
# and a listed file read as unlisted at random (vendor-integrity --self-test failed twice on it, WS-10 CI).
listed() {
    local f
    for f in "${files[@]}"; do
        [ "$f" = "$1" ] && return 0
    done
    return 1
}

# ---- 2. patches -------------------------------------------------------------------------------
mapfile -t patches < <(cd "$dest/patches" && LC_ALL=C ls -1 -- *.patch 2>/dev/null || true)
declare -A patched_by=()
for p in "${patches[@]}"; do
    (cd "$work/stage" && git apply --check "$dest/patches/$p") \
        || die 3 "patches/$p does not apply to $sha (plus the patches before it)"
    while IFS=$'\t' read -r _add _del path; do
        listed "$path" \
            || die 3 "patches/$p touches $path, which FILES does not list"
        patched_by["$path"]="${patched_by[$path]:+${patched_by[$path]}, }${p%%-*}"
    done < <(cd "$work/stage" && git apply --numstat "$dest/patches/$p")
    (cd "$work/stage" && git apply "$dest/patches/$p") || die 3 "patches/$p failed to apply"
done
# GPL-3.0 section 5(a): every file a patch changes gets a notice line from that same patch.
for p in "${patches[@]}"; do
    missing="$(awk -v marker="$notice_marker" '
        /^\+\+\+ b\// { if (file != "" && !seen) print file; file = substr($0, 7); seen = 0; next }
        /^\+/ && index($0, marker) { seen = 1 }
        END { if (file != "" && !seen) print file }' "$dest/patches/$p")"
    [ -z "$missing" ] || die 3 "patches/$p changes $missing without adding a '$notice_marker' notice"
done
for f in "${!patched_by[@]}"; do
    if cmp -s "$work/raw/$f" "$work/stage/$f"; then
        die 3 "$f is touched by ${patched_by[$f]} but ends up identical to upstream"
    fi
done

# ---- 3. the generated theme seed still matches its JSON ---------------------------------------
if [ -f "$work/stage/tools/gen_theme_seed.py" ]; then
    python3 "$work/stage/tools/gen_theme_seed.py" --check >"$work/seed.log" 2>&1 \
        || { cat "$work/seed.log" >&2; die 3 "tools/gen_theme_seed.py --check failed"; }
fi

# ---- 4. PROVENANCE.md -------------------------------------------------------------------------
hash() { sha256sum "$1" | cut -d' ' -f1; }
{
    echo "# Vendored AetherSDR closure: provenance"
    echo
    echo "Generated by \`tools/sync-aethersdr.sh\`; do not edit. \`ci/scripts/vendor-integrity.sh\`"
    echo "re-fetches the upstream bytes, re-applies the patches, regenerates this file and fails on any"
    echo "difference from the committed tree."
    echo
    echo "- Upstream: $upstream_url"
    echo "- Upstream commit: \`$sha\`"
    echo "- Licences: AetherSDR GPL-3.0 (\`LICENSE\`); SQLite public domain (\`third_party/sqlite/README.md\`);"
    echo "  Inter SIL OFL 1.1 (\`third_party/inter/LICENSE.txt\`)."
    echo "- Rule (master §2.2): every file is byte-identical to upstream except where a patch below"
    echo "  changes it; each patch adds a GPL-3.0 §5(a) modification notice to every file it changes."
    echo
    echo "## Patches (applied in this order)"
    echo
    echo "| Patch | SHA-256 |"
    echo "|---|---|"
    for p in "${patches[@]}"; do
        echo "| \`patches/$p\` | \`$(hash "$dest/patches/$p")\` |"
    done
    echo
    echo "## Files"
    echo
    echo "| File | Upstream SHA-256 | Vendored SHA-256 | Patches |"
    echo "|---|---|---|---|"
    for f in "${files[@]}"; do
        up="$(hash "$work/raw/$f")"
        if [ -n "${patched_by[$f]:-}" ]; then
            echo "| \`$f\` | \`$up\` | \`$(hash "$work/stage/$f")\` | ${patched_by[$f]} |"
        else
            echo "| \`$f\` | \`$up\` | same | none |"
        fi
    done
} > "$work/PROVENANCE.md"

# ---- 5. refuse anything unlisted, then install -------------------------------------------------
unlisted=()
while IFS= read -r -d '' path; do
    rel="${path#"$dest"/}"
    case "$rel" in
        FILES|PROVENANCE.md|patches/*.patch) continue ;;
    esac
    listed "$rel" || unlisted+=("$rel")
done < <(find "$dest" \( -type f -o -type l \) -print0)
if [ "${#unlisted[@]}" -gt 0 ]; then
    printf 'sync-aethersdr: UNLISTED file in the vendored tree: %s\n' "${unlisted[@]}" >&2
    exit 4
fi

for f in "${files[@]}"; do
    mkdir -p "$dest/$(dirname "$f")"
    cp "$work/stage/$f" "$dest/$f"
done
cp "$work/PROVENANCE.md" "$dest/PROVENANCE.md"

echo "sync-aethersdr: ${#files[@]} files from $sha, ${#patches[@]} patch(es) applied, into $dest"
