#!/usr/bin/env bash
# Build the release Linux headless binary against the static Qt (WS-13; master D3, D36, section 9).
#
# usage: packaging/linux/build-release.sh release|seams <static-qt-prefix> <build-dir> <out-dir> <arch>
#
#   release  ASB_BUILD_TESTS=OFF (no test seams), Release, -static-libstdc++ -static-libgcc. Writes
#            <out-dir>/aether-serial-bridge-linux-<arch>: the artifact itself.
#   seams    the same sources, Qt and flags with ASB_BUILD_TESTS=ON (the test seams: --test-op, ASB_TEST_*), only
#            the targets the systemd suite needs (aether-serial-bridge, asb_seccomp_probe, tst_HelperRoundTrip,
#            asb_stage_client),
#            copied to <out-dir>/. It is never shipped: the suite produces its failure points with it and runs
#            the whole suite against it, while the shipped bytes run every case that needs no seam.
#
# Run it inside the ubuntu:22.04 build container (ci/containers/build-ubuntu2204.Dockerfile), so the binary needs
# glibc 2.35 at most (INV-87; release.yml audits it with cmake/GlibcAudit.cmake).
set -euo pipefail

mode="${1:?usage: build-release.sh release|seams <static-qt-prefix> <build-dir> <out-dir> <arch>}"
qt="${2:?static Qt prefix}"
build="${3:?build dir}"
out="${4:?out dir}"
arch="${5:?arch (x64 or arm64)}"
src="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

case "$mode" in
    release) tests=OFF ;;
    seams) tests=ON ;;
    *) echo "unknown mode $mode" >&2; exit 2 ;;
esac

# The static Qt must really be static: a shared Qt here would make a binary that needs Qt beside it.
test -f "$qt/lib/libQt6Core.a" || { echo "ERROR: $qt is not a static Qt (no lib/libQt6Core.a)" >&2; exit 1; }

cmake -S "$src" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DASB_BUILD_GUI=OFF -DASB_BUILD_TESTS="$tests" -DASB_WERROR=ON -DASB_REQUIRE_PINNED_QT=ON \
    -DCMAKE_PREFIX_PATH="$qt" \
    "-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc"

mkdir -p "$out"
if [ "$mode" = release ]; then
    cmake --build "$build" --target aether-serial-bridge
    install -m 0755 "$build/bin/aether-serial-bridge" "$out/aether-serial-bridge-linux-$arch"
    echo "built $out/aether-serial-bridge-linux-$arch"
else
    cmake --build "$build" --target aether-serial-bridge asb_seccomp_probe tst_HelperRoundTrip asb_stage_client
    for f in aether-serial-bridge asb_seccomp_probe tst_HelperRoundTrip asb_stage_client; do
        install -m 0755 "$build/bin/$f" "$out/$f"
    done
    echo "built the seam build in $out"
fi
