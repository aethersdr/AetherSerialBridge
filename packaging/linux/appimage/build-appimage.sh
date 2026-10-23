#!/usr/bin/env bash
# Build aether-serial-bridge-gui-<arch>.AppImage for this machine's architecture, x86_64 or aarch64 (WS-13; master
# D3, section 9).
#
# usage: packaging/linux/appimage/build-appimage.sh <qt-prefix> <build-dir> <headless-release-binary> <out-dir> <tools-cache>
#
#   qt-prefix                the pinned DYNAMIC Qt: the window runs unprivileged, so it keeps Qt's own libraries and
#                            plugins inside the image (D3). x86_64: aqt's (ci/scripts/install-qt.sh); aarch64: the
#                            shared Qt built from source (packaging/qt-static/profiles/linux-gui-shared.txt), because
#                            aqt's arm64 QtGui needs GLIBC_2.38.
#   headless-release-binary  aether-serial-bridge-linux-x64 / -arm64 from packaging/linux/build-release.sh. It is embedded as
#                            usr/libexec/aether-serial-bridge AFTER linuxdeploy ran, so nothing rewrites it: the
#                            AppImage's --install (AppRun) installs exactly the released bytes. release.yml extracts
#                            the image and compares the two. Its SHA-256 is compiled into the window (v1.1 D61).
#   tools-cache              where the pinned tools (tools.env) are downloaded and verified.
#
# Run inside the ubuntu:22.04 build container: the window's own objects then need glibc 2.35 at most, like the
# headless binary. The container has no FUSE, so every AppImage tool runs extracted (APPIMAGE_EXTRACT_AND_RUN).
set -euo pipefail

qt="${1:?qt prefix}"
build="${2:?build dir}"
headless="${3:?headless release binary}"
out="${4:?out dir}"
tools="${5:?tools cache}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$(cd "$here/../../.." && pwd)"
# shellcheck disable=SC1091
source "$here/tools.env"
export APPIMAGE_EXTRACT_AND_RUN=1 NO_CLEANUP=1
arch="$(uname -m)"
case "$arch" in x86_64 | aarch64) ;; *) echo "no AppImage tools pinned for $arch" >&2; exit 2 ;; esac
pin() { local v="${1}_${arch}"; printf '%s' "${!v}"; }

fetch() {   # fetch <url> <sha256> <name>
    local file="$tools/$3"
    mkdir -p "$tools"
    if [ ! -f "$file" ] || ! echo "$2  $file" | sha256sum -c --status; then
        rm -f "$file"
        curl -fsSL --retry 3 -o "$file.part" "$1"
        echo "$2  $file.part" | sha256sum -c --status || { echo "ERROR: $3 does not match its pinned SHA-256" >&2; rm -f "$file.part"; exit 1; }
        mv "$file.part" "$file"
    fi
    chmod +x "$file"
}
fetch "$(pin LINUXDEPLOY_URL)" "$(pin LINUXDEPLOY_SHA256)" "linuxdeploy-$arch.AppImage"
fetch "$(pin LINUXDEPLOY_QT_URL)" "$(pin LINUXDEPLOY_QT_SHA256)" "linuxdeploy-plugin-qt-$arch.AppImage"
fetch "$(pin APPIMAGETOOL_URL)" "$(pin APPIMAGETOOL_SHA256)" "appimagetool-$arch.AppImage"
fetch "$(pin RUNTIME_URL)" "$(pin RUNTIME_SHA256)" "runtime-$arch"

# The window: dynamic Qt, no test seams. It compiles in the SHA-256 of the program it embeds (v1.1 D61, INV-121:
# HeadlessDigest.cmake's ASB_HEADLESS_DIGEST_EXE), not of its own tree's development program: the window's build
# identity is the release program's. release.yml checks it with release_audit.py digest.
headless_abs="$(cd "$(dirname "$headless")" && pwd)/$(basename "$headless")"
cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DASB_BUILD_GUI=ON -DASB_BUILD_TESTS=OFF -DASB_WERROR=ON -DASB_REQUIRE_PINNED_QT=ON -DCMAKE_PREFIX_PATH="$qt" \
    -DASB_HEADLESS_DIGEST_EXE="$headless_abs"
cmake --build "$build" --target aether-serial-bridge-gui

appdir="$build/AppDir"
rm -rf "$appdir"
install -D -m 0755 "$build/bin/aether-serial-bridge-gui" "$appdir/usr/bin/aether-serial-bridge-gui"
install -D -m 0644 "$here/aether-serial-bridge.desktop" "$appdir/usr/share/applications/aether-serial-bridge.desktop"
# The application icon (resources/icon/, tools/make_icons.sh), named as the desktop entry's Icon=.
for size in 256 512; do
    install -D -m 0644 "$src/resources/icon/asb-icon-$size.png" \
        "$appdir/usr/share/icons/hicolor/${size}x${size}/apps/aether-serial-bridge.png"
done
install -D -m 0644 "$src/LICENSE" "$appdir/usr/share/doc/aether-serial-bridge/LICENSE"
install -D -m 0644 "$src/packaging/THIRD-PARTY-NOTICES.txt" "$appdir/usr/share/doc/aether-serial-bridge/THIRD-PARTY-NOTICES.txt"

# linuxdeploy: the window's libraries and Qt plugins (xcb, and offscreen for the release self-test's
# --self-test-init). It writes AppRun and apprun-hooks/. No `--output`: the image is packed below, after the headless
# binary is in place.
(cd "$build" && QMAKE="$qt/bin/qmake" LD_LIBRARY_PATH="$qt/lib" PATH="$tools:$PATH" EXTRA_PLATFORM_PLUGINS=libqoffscreen.so \
    "$tools/linuxdeploy-$arch.AppImage" --appdir "$appdir" --plugin qt \
        --executable "$appdir/usr/bin/aether-serial-bridge-gui" \
        --desktop-file "$appdir/usr/share/applications/aether-serial-bridge.desktop" \
        --icon-file "$appdir/usr/share/icons/hicolor/256x256/apps/aether-serial-bridge.png" \
        --icon-file "$appdir/usr/share/icons/hicolor/512x512/apps/aether-serial-bridge.png")

# Our AppRun replaces linuxdeploy's (it dispatches --install to the embedded program and sources the Qt hooks
# for the window itself).
rm -f "$appdir/AppRun" "$appdir/AppRun.wrapped"
install -m 0755 "$here/AppRun" "$appdir/AppRun"
install -D -m 0755 "$headless" "$appdir/usr/libexec/aether-serial-bridge"

mkdir -p "$out"
image="$out/aether-serial-bridge-gui-$arch.AppImage"
rm -f "$image"
ARCH="$arch" "$tools/appimagetool-$arch.AppImage" --no-appstream --runtime-file "$tools/runtime-$arch" "$appdir" "$image"
chmod 0755 "$image"
echo "built $image"
