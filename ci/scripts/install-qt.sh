#!/usr/bin/env bash
# Install the pinned Qt (cmake/qt-pin.env) for Linux with aqtinstall.
#
# usage: ci/scripts/install-qt.sh <qt-root> [linux_gcc_64|linux_gcc_arm64]
#
# Qt lands in <qt-root>/<version>/<gcc_64|gcc_arm64>. The directory is the cache: CI restores it
# with actions/cache, a developer bind-mounts it into the build container. The script is a no-op
# when a complete tree for the pinned version and package revision is already there.
#
# The stamp is install evidence: Qt republishes a version under a new package revision and aqt
# installs whatever is current, so after aqt has run, check_qt_revision.py reads aqt's log and
# refuses to write the stamp unless every archive carries QT_PACKAGE_REVISION. A cached tree is
# trusted only when its stamp holds that proof. A mismatch fails the install and removes the tree.
#
# Only qtbase plus the SerialPort module are installed. The pin model is AetherSDR's: aqt itself is
# pinned, because it decides what Qt lands here.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$here/../../cmake/qt-pin.env"

qt_root="${1:?usage: install-qt.sh <qt-root> [qt-arch]}"
qt_arch="${2:-linux_gcc_64}"
# Absolute from here on: the script changes directory before aqt runs, and the prefix, the stamp
# and the venv are all derived from this path.
mkdir -p "$qt_root"
qt_root="$(cd "$qt_root" && pwd)"
kit="${qt_arch#linux_}"
prefix="$qt_root/$QT_VERSION/$kit"
stamp="$qt_root/.installed-$QT_VERSION-$QT_PACKAGE_REVISION-$kit-aqt$AQTINSTALL_VERSION"

check_revision() { python3 "$here/check_qt_revision.py" "$@"; }

verify() {
    test -x "$prefix/bin/qmake" || return 1
    test -f "$prefix/lib/cmake/Qt6/Qt6Config.cmake" || return 1
    test -f "$prefix/lib/cmake/Qt6SerialPort/Qt6SerialPortConfig.cmake" || return 1
    test -f "$prefix/lib/cmake/Qt6DBus/Qt6DBusConfig.cmake" || return 1
    test -f "$prefix/lib/cmake/Qt6Test/Qt6TestConfig.cmake" || return 1
    "$prefix/bin/qmake" -query QT_VERSION | grep -qx "$QT_VERSION"
}

if [ -e "$stamp" ] && verify && check_revision evidence --file "$stamp" \
        --version "$QT_VERSION" --revision "$QT_PACKAGE_REVISION"; then
    echo "Qt $QT_VERSION ($kit) already installed at $prefix"
    exit 0
fi

venv="$(mktemp -d "$qt_root/.aqt-venv.XXXXXX")"
trap 'rm -rf "$venv"' EXIT
python3 -m venv "$venv"
"$venv/bin/pip" install --no-cache-dir --quiet "aqtinstall==$AQTINSTALL_VERSION"
# Everything aqt needs is in place: only now drop the previous (possibly broken) prefix, and any
# stamp that vouched for it.
rm -rf "$prefix"
rm -f "$stamp"
# aqt appends its log to aqtinstall.log in the working directory: keep it out of the source tree,
# and start it empty so the revision check reads this install and nothing older.
cd "$qt_root"
rm -f aqtinstall.log
"$venv/bin/aqt" install-qt linux desktop "$QT_VERSION" "$qt_arch" \
    -m qtserialport \
    --outputdir "$qt_root"

# qmake alone only proves qtbase landed: assert the CMake packages the build asks for.
verify || { echo "ERROR: Qt tree at $prefix is incomplete after aqt install" >&2; ls -la "$qt_root/$QT_VERSION" >&2 || true; exit 1; }
# Which revision did aqt just install? Fails (and the stamp is not written) unless it is the pin.
if ! check_revision install --log "$qt_root/aqtinstall.log" \
        --version "$QT_VERSION" --revision "$QT_PACKAGE_REVISION" --out "$stamp.tmp"; then
    rm -rf "$prefix" "$stamp.tmp"
    exit 1
fi
mv "$stamp.tmp" "$stamp"
echo "Qt $("$prefix/bin/qmake" -query QT_VERSION) installed at $prefix"
