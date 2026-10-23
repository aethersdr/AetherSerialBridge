#!/usr/bin/env bash
# The host-libs gate's control (v1.1 fix, the window on stock Ubuntu): a copy of the extracted AppImage whose window
# was given `patchelf --add-needed libOpenGL.so.0` (the shape of the bug: the window NEEDed it, and neither the image
# nor a stock Ubuntu desktop has it) must FAIL `release_audit.py host-libs`, and the walk must name libOpenGL.so.0.
#
# usage: tools/host-libs-control.sh <squashfs-root> <allowlist> <linuxdeploy-AppImage> [<out-dir>]
#
# patchelf is the one inside the SHA-256-pinned linuxdeploy AppImage (packaging/linux/appimage/tools.env, the tools
# cache): no download, and no change to the build image. With <out-dir>, the planted window is also left there as
# <out-dir>/aether-serial-bridge-gui (the stock-desktop start's control uses it).
set -euo pipefail
root="${1:?squashfs-root}"
allowlist="${2:?allowlist}"
linuxdeploy="${3:?linuxdeploy AppImage}"
out="${4:-}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
allowlist="$(cd "$(dirname "$allowlist")" && pwd)/$(basename "$allowlist")"
linuxdeploy="$(cd "$(dirname "$linuxdeploy")" && pwd)/$(basename "$linuxdeploy")"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
(cd "$tmp" && APPIMAGE_EXTRACT_AND_RUN=1 "$linuxdeploy" --appimage-extract usr/bin/patchelf >/dev/null)
patchelf="$tmp/squashfs-root/usr/bin/patchelf"
test -x "$patchelf" || { echo "FAIL control: no patchelf in $linuxdeploy" >&2; exit 1; }
cp -a "$root" "$tmp/tree"
gui="$tmp/tree/usr/bin/aether-serial-bridge-gui"
"$patchelf" --add-needed libOpenGL.so.0 "$gui"
if [ -n "$out" ]; then
    mkdir -p "$out"
    cp "$gui" "$out/aether-serial-bridge-gui"
fi
# Fail closed: the control passes only when the walk refuses the tree AND names the planted library (a walk that
# refuses for another reason proves nothing about this one).
rc=0
python3 "$here/release_audit.py" host-libs-control "$tmp/tree" "$allowlist" | tee "$tmp/out" || rc=$?
if [ "$rc" -ne 0 ] || ! grep -q "refuses .*libOpenGL\.so\.0" "$tmp/out"; then
    echo "FAIL control: the host-libs walk did not refuse the window that needs libOpenGL.so.0" >&2
    exit 1
fi
