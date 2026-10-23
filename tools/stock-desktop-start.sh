#!/usr/bin/env bash
# The window starts on a STOCK Ubuntu desktop userland (v1.1 fix): the AppImage, extracted inside
# asb-stock-desktop-2604 (ci/containers/stock-desktop-ubuntu2604.Dockerfile: ubuntu-desktop-minimal, nothing of ours),
# runs `--self-test-init` on the offscreen platform and must report the window exposed and painted
# (release_audit.py gui-report), exit status included. The build image asb-build-2204 has every GL library, so the
# Gates step's self-test there could not see a missing host library (libOpenGL.so.0, 2026-10-19); this one can.
#
# Control, fail closed: the same tree with the window replaced by <planted-window> (tools/host-libs-control.sh's,
# `patchelf --add-needed libOpenGL.so.0`) must FAIL to start here, and the loader must name libOpenGL.so.0. Any other
# outcome (it starts, or fails for another reason) means this userland does not reproduce the bug, and the gate fails.
#
# The userland itself is checked first: libGL.so.1 and libEGL.so.1 present, libOpenGL.so.0 absent (else: "the stock
# userland changed").
#
# usage: tools/stock-desktop-start.sh <image> <AppImage> <planted-window> <work-dir>
#
# <work-dir> must be a path the Docker daemon can bind (on the runners, under the workspace). The container is capped
# at 2g / 2 CPUs and labelled org.aethersdr.asb-ci, like everything CI starts.
set -euo pipefail
image="${1:?image}"
appimage="${2:?AppImage}"
planted="${3:?planted window}"
work="${4:?work dir}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
abs() { echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; }
appimage="$(abs "$appimage")"
planted="$(abs "$planted")"
rm -rf "$work" && mkdir -p "$work"
work="$(cd "$work" && pwd)"

# One container, two runs. As the caller's uid (the image's desktop user is the default; -u keeps /out writable).
docker run --rm --label org.aethersdr.asb-ci=containers --memory 2g --cpus 2 --network none \
    -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$appimage:/in/app.AppImage:ro" -v "$planted:/in/planted:ro" \
    -v "$work:/out" "$image" bash -uo pipefail -c '
        cd /tmp
        ldconfig -p | grep -oE "lib(OpenGL|GLX|GL|EGL)\.so\.[0-9]+" | sort -u > /out/glvnd.txt
        echo "userland: $(. /etc/os-release && echo "$PRETTY_NAME"), glvnd: $(tr "\n" " " < /out/glvnd.txt)" | tee /out/userland.txt
        /in/app.AppImage --appimage-extract >/dev/null || { echo "could not extract the AppImage" >&2; exit 3; }
        rc=0; QT_QPA_PLATFORM=offscreen timeout -k 10 120 squashfs-root/AppRun --self-test-init \
            > /out/selftest.out 2> /out/selftest.err || rc=$?
        echo "$rc" > /out/selftest.rc
        cp /in/planted squashfs-root/usr/bin/aether-serial-bridge-gui
        rc=0; QT_QPA_PLATFORM=offscreen timeout -k 10 120 squashfs-root/AppRun --self-test-init \
            > /out/control.out 2> /out/control.err || rc=$?
        echo "$rc" > /out/control.rc
    '

bad=0
# The facts this gate rests on: a stock desktop has glvnd's libGL.so.1 and libEGL.so.1 and NOT
# libOpenGL.so.0. If the image stops matching that (a bumped base, a changed seed), say so: a result on another
# userland is not evidence about a stock desktop, so the gate fails with that diagnosis rather than a window verdict.
for want in libGL.so.1 libEGL.so.1; do
    grep -qx "$want" "$work/glvnd.txt" || { echo "FAIL the stock userland changed: $want is missing ($(cat "$work/userland.txt")); rebuild or re-pin ci/containers/stock-desktop-ubuntu2604.Dockerfile"; bad=1; }
done
if grep -qx libOpenGL.so.0 "$work/glvnd.txt"; then
    echo "FAIL the stock userland changed: libOpenGL.so.0 is installed ($(cat "$work/userland.txt")), so it can no longer catch a window that needs it"
    bad=1
fi
echo "--- the window, from the image, on the stock userland"
tail -n 5 "$work/selftest.err" || true
python3 "$here/release_audit.py" gui-report "$work/selftest.out" "$(cat "$work/selftest.rc")" || bad=1
echo "--- control: the window that needs libOpenGL.so.0, on the same userland"
crc="$(cat "$work/control.rc")"
tail -n 5 "$work/control.err" || true
if [ "$crc" != 0 ] && grep -q "libOpenGL\.so\.0: cannot open shared object file" "$work/control.err"; then
    echo "ok   control: the planted window does not start here (exit $crc): the loader cannot find libOpenGL.so.0"
else
    echo "FAIL control: the planted window exited $crc without the loader naming libOpenGL.so.0; this userland does not reproduce the bug, so the start above proves nothing"
    bad=1
fi
exit "$bad"
