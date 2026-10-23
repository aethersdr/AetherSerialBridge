#!/usr/bin/env python3
"""The systemd suite (WS-8): black-box cases against systemd as PID 1 in the debian:bookworm container.

usage: tests/systemd/driver.py --bin <dir> --qt <qt prefix>|none [--qt-version V] [--image IMAGE]
                               [--work <dir on ext4>] [--release --seam-bin <dir>] [--dist <dir>] [--fixtures <dir>]
                               [--list] [CASE ...]

<dir> holds the built aether-serial-bridge (test build: --test-op) and asb_seccomp_probe. CASE selects
cases by exact name or prefix ("lock_pair_crash"). `--qt none`: a static build needs no Qt tree.

--release (WS-13): <dir> holds the RELEASE binary (renamed aether-serial-bridge) and --seam-bin the seam build of
the same commit and Qt (aether-serial-bridge, asb_seccomp_probe, tst_HelperRoundTrip); see sysd.py for what runs
where. Cases registered release=False are skipped. Each result line says which program the case's service ran
("installed: release" when /usr/local/bin/aether-serial-bridge was byte-identical to the release binary at the
end). --gui-bin (v1.1 WS-17) mounts a seam-build window (aether-serial-bridge-gui, dynamic Qt with the offscreen
plugin) and the program beside it at /usr/lib/asb-test/gui; without it, <dir> is mounted there when it holds the window.
--dist mounts the release artifacts at /usr/lib/asb-test/dist (the AppImage case). --fixtures mounts the pinned
program fixtures (v1.1: asb-fixtures/v1.0/...) at /usr/lib/asb-test/fixtures; tests/systemd/fixtures.env holds the pins.

Output is QtTest-shaped:
    PASS   : sysd::<case>()
    FAIL!  : sysd::<case>() <reason>
Exit status 1 when any case failed.

Each case boots its own privileged container (systemd needs it; only this container is privileged, as in
the containers CI leg) and removes it afterwards.
"""

import argparse
import concurrent.futures
import hashlib
import importlib
import io
import threading
import os
import pathlib
import subprocess
import sys
import time
import traceback

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sysd  # noqa: E402


def load_cases():
    for f in sorted((HERE / "cases").glob("*.py")):
        if f.name.startswith("_"):
            continue
        importlib.import_module("cases." + f.stem)
    return sysd.REGISTRY


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=False)
    ap.add_argument("--qt", required=False)
    ap.add_argument("--qt-version", default=None)
    ap.add_argument("--image", default=os.environ.get("ASB_SYSTEMD_IMAGE", "asb-systemd-bookworm"))
    ap.add_argument("--fedora-image", default=os.environ.get("ASB_FEDORA_IMAGE", "asb-polkit-fedora"),
                    help="v1.1 WS-15: the Fedora polkit image (tests/polkit/fedora.Dockerfile) for kind=fedora cases")
    ap.add_argument("--work", default=None)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--release", action="store_true")
    ap.add_argument("--seam-bin", default=None)
    ap.add_argument("--dist", default=None)
    ap.add_argument("--fixtures", default=None)
    ap.add_argument("--gui-bin", default=None, help="v1.1 WS-17: the seam window's build (window_lifecycle_native)")
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("ASB_SYSD_JOBS", "1")),
                    help="cases run in parallel, each in its own container")
    ap.add_argument("cases", nargs="*")
    a = ap.parse_args()
    reg = load_cases()
    names = sorted(reg)
    if a.cases:
        names = [n for n in names if any(n == c or n.startswith(c) for c in a.cases)]
        if not names:
            print("no case matches %s" % a.cases, file=sys.stderr)
            return 2
    if a.list:
        print("\n".join(names))
        return 0
    if not a.bin or not a.qt:
        ap.error("--bin and --qt are required")
    if a.release and not a.seam_bin:
        ap.error("--release needs --seam-bin")
    sysd.RELEASE = a.release
    qt_version = a.qt_version
    if not qt_version:
        pin = (HERE.parent.parent / "cmake" / "qt-pin.env").read_text()
        qt_version = [l.split("=", 1)[1].strip() for l in pin.splitlines() if l.startswith("QT_VERSION=")][0]
    work = os.path.abspath(a.work or os.path.join(a.bin, "..", "sysd-work"))
    os.makedirs(work, exist_ok=True)
    bindir = os.path.abspath(a.bin)
    qt = None if a.qt == "none" else os.path.abspath(a.qt)
    seam = os.path.abspath(a.seam_bin) if a.seam_bin else None
    mounts = [(os.path.abspath(a.dist), sysd.DIST)] if a.dist else []
    if a.fixtures and os.path.isdir(a.fixtures):
        mounts.append((os.path.abspath(a.fixtures), sysd.FIXTURES))
    gui = a.gui_bin or (bindir if os.path.exists(os.path.join(bindir, "aether-serial-bridge-gui")) else None)
    if gui:
        mounts.append((os.path.abspath(gui), sysd.GUI_DIR))
    release_sha = None
    if a.release:
        with open(os.path.join(bindir, "aether-serial-bridge"), "rb") as f:
            release_sha = hashlib.sha256(f.read()).hexdigest()

    counts = {"passed": 0, "failed": 0, "skipped": 0, "release-installed": 0}
    lock = threading.Lock()
    t0 = time.time()

    def run_one(n):
        fn, lockdir, release_ok = reg[n]
        safe = "".join(ch if ch.isalnum() else "-" for ch in n)[:40]
        shown = "sysd::%s" % n if n.endswith(")") else "sysd::%s()" % n   # QtTest: f() or f(tag)
        # Unique per case even when two names share their first characters (data rows) and cases run in
        # parallel: the container name and the host lock directory both come from it.
        tag = hashlib.sha1(n.encode()).hexdigest()[:8]
        if a.release and not release_ok:
            with lock:
                counts["skipped"] += 1
                print("SKIP   : %s release mode: the case exercises the seam build only (it runs in the seam-build pass)" % shown,
                      flush=True)
            return
        if sysd.KINDS.get(n) == "fedora":
            if subprocess.run(["docker", "image", "inspect", a.fedora_image], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode != 0:
                with lock:
                    counts["skipped"] += 1
                    print("SKIP   : %s the Fedora polkit image %s is not built (tests/polkit/fedora.Dockerfile)"
                          % (shown, a.fedora_image), flush=True)
                return
            box = sysd.PolkitBox(a.fedora_image, bindir, qt, qt_version, work, "asb-sysd-%s-%s-%d" % (safe[:30], tag, os.getpid()),
                                 lockdir_mount=False, seam_dir=seam, extra_mounts=mounts)
        else:
            box = sysd.Box(a.image, bindir, qt, qt_version, work, "asb-sysd-%s-%s-%d" % (safe[:30], tag, os.getpid()),
                           lockdir_mount=lockdir, seam_dir=seam, extra_mounts=mounts)
        start = time.time()
        log = io.StringIO()
        failed = False
        installed = ""
        try:
            box.boot()
            fn(box)
            if a.release:
                r = box.run("sha256sum %s 2>/dev/null | cut -d' ' -f1" % sysd.INSTALLED)
                got = r.out.strip()
                installed = " [installed: %s]" % ("release" if got == release_sha else "none" if not got else "seam build")
            line, key = "PASS   : %s (%.1fs)%s" % (shown, time.time() - start, installed), "passed"
        except sysd.CaseSkip as e:
            line, key = "SKIP   : %s %s" % (shown, e), "skipped"
        except sysd.CaseFail as e:
            line, key, failed = "FAIL!  : %s %s" % (shown, str(e).replace("\n", " | ")[:3000]), "failed", True
        except Exception as e:  # a harness error is a failure of the case, with its traceback
            line, key, failed = "FAIL!  : %s harness error: %s" % (shown, e), "failed", True
            traceback.print_exc(file=log)
        finally:
            if failed and os.environ.get("ASB_SYSD_KEEP"):
                log.write("   kept container %s\n" % box.name)
            else:
                box.destroy()
        with lock:
            counts[key] += 1
            if installed == " [installed: release]":
                counts["release-installed"] += 1
            print(line, flush=True)
            if log.getvalue():
                print(log.getvalue(), end="", flush=True)

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as pool:
        list(pool.map(run_one, names))
    passed, failed, skipped = counts["passed"], counts["failed"], counts["skipped"]
    print("Totals: %d passed, %d failed, %d skipped (%.0fs)" % (passed, failed, skipped, time.time() - t0))
    if a.release:
        print("Release mode: %d passing cases ended with the release binary installed" % counts["release-installed"])
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
