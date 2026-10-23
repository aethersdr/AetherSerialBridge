#!/usr/bin/env bash
# Build the pinned Qt (cmake/qt-pin.env) from source for one profile (WS-13; master D3, section 9): a STATIC Qt for the
# release binaries, or, for a profile that says -shared (linux-gui-shared: the arm64 window's AppImage), a shared one.
#
# usage: packaging/qt-static/build-qt-static.sh [--key] <profile> <cache-root>
#
#   --key       print the key and the prefix (QT_STATIC_KEY=, QT_STATIC_PREFIX=) and exit, building nothing. CI
#               keys its actions/cache entry of the prefix with it, so an entry holds exactly the tree its stamp
#               vouches for (run it where the build runs: the key hashes that compiler).
#   profile     packaging/qt-static/profiles/<profile>.txt (Linux: linux-headless). Run it inside the
#               ubuntu:22.04 build container (ci/containers/build-ubuntu2204.Dockerfile): the release
#               binary's glibc floor is that image's 2.35 (D36, INV-87).
#   cache-root  a directory that persists between runs. The finished Qt lands in
#               <cache-root>/<QT_VERSION>-<profile>-<key>, where <key> hashes everything that decides the
#               bytes: the version, the source archives' SHA-256, the profile's configure arguments, this
#               script, the compiler and the machine architecture. A second run with the same key is a no-op.
#
# The last line printed is QT_STATIC_PREFIX=<prefix>. The tree is trusted only through its stamp, written
# after both modules installed and the checks below passed, holding the exact key inputs; a tree without
# a matching stamp is rebuilt from scratch. Source archives are verified against packaging/qt-static/
# qt-static.env before anything is extracted, and are kept in <cache-root>/src.
#
# Parallelism: ASB_QT_JOBS (default: nproc). Static Qt takes 25-60 min cold.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
# shellcheck disable=SC1091
source "$root/cmake/qt-pin.env"
# shellcheck disable=SC1091
source "$here/qt-static.env"

key_only=0
if [ "${1:-}" = "--key" ]; then key_only=1; shift; fi
profile="${1:?usage: build-qt-static.sh [--key] <profile> <cache-root>}"
cache="${2:?usage: build-qt-static.sh [--key] <profile> <cache-root>}"
profile_file="$here/profiles/$profile.txt"
test -f "$profile_file" || { echo "ERROR: no profile $profile_file" >&2; exit 2; }
mkdir -p "$cache"
cache="$(cd "$cache" && pwd)"
jobs="${ASB_QT_JOBS:-$(nproc)}"

# The configure arguments: every non-comment line, split on whitespace.
args=()
while read -r line; do
    line="${line%%#*}"
    [ -n "${line// /}" ] || continue
    read -ra words <<<"$line"
    args+=("${words[@]}")
done <"$profile_file"

compiler="$("${CXX:-c++}" --version | head -1)"
key_input="$(printf 'qt %s\nqtbase %s\nqtserialport %s\nprofile %s\nargs %s\nscript %s\ncompiler %s\narch %s\n' \
    "$QT_VERSION" "$QTBASE_TAR_SHA256" "$QTSERIALPORT_TAR_SHA256" "$profile" "${args[*]}" \
    "$(sha256sum "${BASH_SOURCE[0]}" | cut -d' ' -f1)" "$compiler" "$(uname -m)")"
key="$(printf '%s\n' "$key_input" | sha256sum | cut -c1-16)"
prefix="$cache/$QT_VERSION-$profile-$key"
stamp="$prefix/.asb-qt-static"
if [ "$key_only" = 1 ]; then
    echo "QT_STATIC_KEY=$key"
    echo "QT_STATIC_PREFIX=$prefix"
    exit 0
fi

shared=0
case " ${args[*]} " in *" -shared "*) shared=1 ;; esac

verify() {
    if [ "$shared" = 1 ]; then
        test -f "$prefix/lib/libQt6Core.so.$QT_VERSION" || return 1         # shared, as the profile says
        test -f "$prefix/lib/libQt6Widgets.so.$QT_VERSION" || return 1
        test -f "$prefix/plugins/platforms/libqxcb.so" || return 1
    else
        test -f "$prefix/lib/libQt6Core.a" || return 1                  # static, not shared
        ! ls "$prefix"/lib/libQt6Core.so* >/dev/null 2>&1 || return 1
    fi
    test -f "$prefix/lib/cmake/Qt6/Qt6Config.cmake" || return 1
    test -f "$prefix/lib/cmake/Qt6SerialPort/Qt6SerialPortConfig.cmake" || return 1
    "$prefix/bin/qmake" -query QT_VERSION | grep -qx "$QT_VERSION"
}

if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$key_input" ] && verify; then
    echo "static Qt $QT_VERSION ($profile, key $key) already built"
    echo "QT_STATIC_PREFIX=$prefix"
    exit 0
fi

echo "building Qt $QT_VERSION from source, profile $profile, key $key, $jobs jobs"
printf '%s\n' "$key_input"

fetch() {   # fetch <archive> <sha256>: into $cache/src, verified; a mismatching file is removed, never used
    local name="$1" sha="$2" file="$cache/src/$1"
    mkdir -p "$cache/src"
    if [ ! -f "$file" ] || ! echo "$sha  $file" | sha256sum -c --status; then
        rm -f "$file" "$file.part"
        curl -fsSL --retry 3 -o "$file.part" "$QT_STATIC_BASE_URL/$name"
        if ! echo "$sha  $file.part" | sha256sum -c --status; then
            echo "ERROR: $name does not match its pinned SHA-256 ($sha)" >&2
            rm -f "$file.part"
            exit 1
        fi
        mv "$file.part" "$file"
    fi
    echo "$file"
}

base_archive="$(fetch "$QTBASE_TAR" "$QTBASE_TAR_SHA256" | tail -1)"
sp_archive="$(fetch "$QTSERIALPORT_TAR" "$QTSERIALPORT_TAR_SHA256" | tail -1)"

work="$(mktemp -d "$cache/.work-$key.XXXXXX")"
trap 'rm -rf "$work"' EXIT
tar -xf "$base_archive" -C "$work"
tar -xf "$sp_archive" -C "$work"
base_src="$work/${QTBASE_TAR%.tar.xz}"
sp_src="$work/${QTSERIALPORT_TAR%.tar.xz}"

# Only now drop a previous (possibly half-built) tree and the stamp that vouched for it.
rm -rf "$prefix"

mkdir -p "$work/build-qtbase"
(cd "$work/build-qtbase" && "$base_src/configure" -prefix "$prefix" "${args[@]}")
cmake --build "$work/build-qtbase" --parallel "$jobs"
cmake --install "$work/build-qtbase"

mkdir -p "$work/build-qtserialport"
(cd "$work/build-qtserialport" && "$prefix/bin/qt-configure-module" "$sp_src")
cmake --build "$work/build-qtserialport" --parallel "$jobs"
cmake --install "$work/build-qtserialport"

verify || { echo "ERROR: the static Qt at $prefix is incomplete" >&2; ls -la "$prefix/lib" >&2 || true; exit 1; }
printf '%s' "$key_input" >"$stamp.tmp"
mv "$stamp.tmp" "$stamp"
echo "static Qt $("$prefix/bin/qmake" -query QT_VERSION) built at $prefix"
echo "QT_STATIC_PREFIX=$prefix"
