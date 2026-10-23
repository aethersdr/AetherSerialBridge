#!/usr/bin/env bash
# Run the BUILT headless executable's --help and --status inside the debian:bookworm systemd
# container (ci/containers/systemd-bookworm.Dockerfile).
#
# usage: ci/scripts/systemd-smoke.sh <headless-exe> <qt-prefix> [--boot]
#
#   <headless-exe>  the built aether-serial-bridge
#   <qt-prefix>     the Qt tree it was built against (contains lib/); mounted read-only at
#                   /qt/<QT_VERSION>/gcc_64, the in-container path the build container configures
#                   with (-DCMAKE_PREFIX_PATH), so the binary's own RUNPATH resolves. Nothing sets
#                   LD_LIBRARY_PATH: a systemd unit (WS-8) has none either.
#   --boot          boot systemd as PID 1 first (needs a privileged container: CI only), then
#                   run the checks with `docker exec`. Without it the checks run as the caller's
#                   uid in a plain container, which is how a developer machine verifies them.
#
# Prints the output of both commands and fails if either exits non-zero or prints nothing.
set -euo pipefail

exe="$(realpath "${1:?usage: systemd-smoke.sh <headless-exe> <qt-prefix> [--boot]}")"
qt="$(realpath "${2:?usage: systemd-smoke.sh <headless-exe> <qt-prefix> [--boot]}")"
mode="${3:-}"
image="${ASB_SYSTEMD_IMAGE:-asb-systemd-bookworm}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$here/../../cmake/qt-pin.env"
qt_in_container="/qt/$QT_VERSION/gcc_64"

if ! docker image inspect "$image" >/dev/null 2>&1; then
    docker build -q --label org.aethersdr.asb-ci=containers -t "$image" -f "$here/../containers/systemd-bookworm.Dockerfile" "$here/../containers"
fi

mounts=(-v "$exe:/usr/local/bin/aether-serial-bridge:ro" -v "$qt:$qt_in_container:ro")
checks='set -eu
ldd_missing=$(ldd /usr/local/bin/aether-serial-bridge | grep "not found" || true)
if [ -n "$ldd_missing" ]; then echo "unresolved libraries:"; echo "$ldd_missing"; exit 1; fi
echo "--- aether-serial-bridge --help"
out=$(/usr/local/bin/aether-serial-bridge --help); echo "$out"; test -n "$out"
echo "--- aether-serial-bridge --status"
out=$(/usr/local/bin/aether-serial-bridge --status); echo "$out"; test -n "$out"
echo "smoke ok: glibc $(ldd --version | head -1)"'

if [ "$mode" = "--boot" ]; then
    name="asb-smoke-$$"
    cleanup() { docker rm -fv "$name" >/dev/null 2>&1 || true; }
    trap cleanup EXIT
    # Labelled (org.aethersdr.asb-ci: everything CI starts carries it) and capped.
    docker run -d --rm --name "$name" --label org.aethersdr.asb-ci=containers --memory 2g --cpus 2 \
        --privileged --cgroupns=private \
        --tmpfs /run --tmpfs /run/lock --tmpfs /tmp "${mounts[@]}" "$image" >/dev/null
    for _ in $(seq 1 60); do
        state="$(docker exec "$name" systemctl is-system-running 2>/dev/null || true)"
        case "$state" in running|degraded) break ;; esac
        sleep 1
    done
    echo "systemd state: ${state:-unknown}"
    case "${state:-}" in running|degraded) ;; *) echo "systemd did not boot" >&2; exit 1 ;; esac
    docker exec "$name" sh -c "$checks"
else
    docker run --rm --label org.aethersdr.asb-ci=containers -u "$(id -u):$(id -g)" "${mounts[@]}" "$image" sh -c "$checks"
fi
