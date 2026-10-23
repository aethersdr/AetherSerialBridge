"""The REAL Linux window through the REAL pkexec (v1.1 WS-17, INV-136, INV-128 on Linux).

The seam-build window (aether-serial-bridge-gui, dynamic Qt, the offscreen platform plugin; driver.py --gui-bin) runs
as uid 1000 in the systemd container with YES rules for both polkit actions (the generic exec action restricted to
/bin/sh, for the staged route; the bridge's helper action, for the installed helper). ASB_TEST_GUI_OPS drives its own
buttons' code (BridgeWindow::installService, saveAndApply, stopService, startService, uninstallService), each answered
from ASB_TEST_GUI_ANSWERS, and the window prints one "asb-gui-op <op> <Outcome>: <message>" line per result. After every
operation the window waits (the "wait" op, released through ASB_TEST_GUI_CONTINUE) while the case reads the system:
the unit, the installed program's digest (this window's own program, staged, D60), the config bytes, the polkit policy,
an empty staging root, and the window process's open files (/proc/<pid>/fd): none under the data folder, the lock
directory or the staging root between operations (INV-128). The window failing to start fails the case (never a skip).
"""

import hashlib
import json
import shlex
import threading
import time

import sysd
from sysd import CONFIG_A, DATA, GUI_DIR, INSTALLED, LOCKDIR, CaseFail, case, expect, running_with
from cases.bootstrap import POLICY, ROOT, UNIT_FILE, exec_rule, expect_root_clean, put, sha_of

WINDOW = GUI_DIR + "/aether-serial-bridge-gui"
PROGRAM = GUI_DIR + "/aether-serial-bridge"   # the window's own program (WS-14's windowProgram(): beside it)
CONTINUE = "/tmp/asb-gui-continue"
# Each operation, then a pause for the case's checks. The second uninstall finishes one that kept the program:
# The case puts the program back at its installed path, with no unit, before it. The last op installs from
# the main button on the clean machine (v1.1 fix after WS-17): it reads "Install and apply" (the box held ticked: the
# Linux window cannot save without installing) and runs the staged install; before the fix it ran the installed
# helper's deactivate, which is not there.
OPS = ["install", "save", "stop", "start", "uninstall", "uninstall", "save"]
LEFT_BEHIND_AT = 4   # the op after which the program is put back
HELD_UNDER = (DATA, LOCKDIR, ROOT)
# CONFIG_A as the window saves it: the product writes every port field, the serial defaults included.
SERIAL_DEFAULTS = {"dataBits": 8, "parity": "None", "stopBits": "One", "handshake": "None", "dtrEnable": False,
                   "rtsEnable": False}
SAVED_A = {"version": CONFIG_A["version"], "ports": [dict(SERIAL_DEFAULTS, **p) for p in CONFIG_A["ports"]]}


class WindowRun:
    """The window process, its output lines read on a thread."""

    def __init__(self, box, env):
        self.box = box
        assigns = " ".join("%s=%s" % (k, shlex.quote(v)) for k, v in env.items())
        self.proc = box.bg("runuser -u tuser -- env %s %s 2>&1" % (assigns, WINDOW))
        self.lines = []
        self.cv = threading.Condition()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for raw in iter(self.proc.stdout.readline, b""):
            with self.cv:
                self.lines.append(raw.decode(errors="replace").rstrip("\n"))
                self.cv.notify_all()
        with self.cv:
            self.cv.notify_all()

    def wait_line(self, prefix, start=0, timeout=240):
        end = time.time() + timeout
        with self.cv:
            while True:
                for i in range(start, len(self.lines)):
                    if self.lines[i].startswith(prefix):
                        return i
                if self.proc.poll() is not None and not self.reader.is_alive():
                    raise CaseFail("the window ended (rc=%s) before %r:\n%s" % (self.proc.returncode, prefix, self.tail()))
                left = end - time.time()
                if left <= 0:
                    raise CaseFail("no %r from the window within %ss:\n%s" % (prefix, timeout, self.tail()))
                self.cv.wait(min(left, 1.0))

    def tail(self, n=40):
        return "\n".join(self.lines[-n:])

    def stop(self):
        if self.proc.poll() is None:
            self.box.run("pkill -u tuser -f %s || true" % shlex.quote(WINDOW), timeout=30)
            try:
                self.proc.wait(timeout=20)
            except Exception:
                self.proc.kill()


def window_pid(box):
    out = box.run("pgrep -u tuser -f %s" % shlex.quote(WINDOW)).out.split()
    expect(out, "the window process is not running")
    return out[0]


def held_files(box, pid):
    """The window's open files under the data folder, the lock directory and the staging root (INV-128)."""
    out = box.run("for f in /proc/%s/fd/*; do readlink \"$f\"; done 2>/dev/null" % pid).out.split("\n")
    return [p for p in out if any(p == d or p.startswith(d + "/") for d in HELD_UNDER)]


@case("window_lifecycle_native", release=False)
def window_lifecycle_native(box):
    """Install -> Save & apply -> Stop -> Start -> Uninstall -> (the program put back) Uninstall -> Install and apply (the
    main button on the clean machine), from the real
    window, through the real pkexec and the staged helper; the system's state and the window's open files after each."""
    expect(box.exists(WINDOW), "no seam window at %s: the containers leg builds it (driver.py --gui-bin)" % WINDOW)
    expect(box.exists(PROGRAM), "no program beside the window at %s" % PROGRAM)
    exec_rule(box, "YES", helper_answer="YES")
    # The settings the window shows and installs: a trusted data folder with one port (nothing installed yet).
    box.sh("install -d -o root -g root -m 755 %s" % DATA)
    config = json.dumps(CONFIG_A, indent=2).encode() + b"\n"
    put(box, DATA + "/config.json", config)
    box.sh("chown root:root %s/config.json" % DATA)
    want_program = sha_of(box, PROGRAM)

    ops = []
    for op in OPS:
        ops += [op, "wait"]
    env = {"HOME": "/home/tuser", "QT_QPA_PLATFORM": "offscreen", "DISPLAY": ":99", "XDG_RUNTIME_DIR": "/tmp/xdg-tuser",
           "ASB_TEST_GUI_OPS": ",".join(ops), "ASB_TEST_GUI_CONTINUE": CONTINUE,
           "ASB_TEST_GUI_ANSWERS": "install:install,update:update,uninstall:uninstall,exposure=1",
           "ASB_TEST_GUI_QUIT_MS": "900000"}
    box.sh("install -d -o tuser -m 700 /tmp/xdg-tuser")
    w = WindowRun(box, env)
    try:
        at = 0
        for n, op in enumerate(OPS):
            i = w.wait_line("asb-gui-op %s " % op, at, timeout=300)
            line = w.lines[i]
            expect(line.startswith("asb-gui-op %s Ok:" % op), "%s (op %d): %s\n%s" % (op, n, line, w.tail()))
            w.wait_line("asb-gui-op wait", i + 1, timeout=60)
            # The checks, while the window waits.
            if op in ("install", "save", "start"):
                running_with(box, CONFIG_A["ports"][0]["tcpPort"])
                expect(sha_of(box, INSTALLED) == want_program, "%s: the installed program is not the window's own" % op)
                saved = box.read(DATA + "/config.json")
                expect(json.loads(saved) == SAVED_A, "%s: config.json is not the window's settings:\n%s" % (op, saved))
                expect(box.exists(POLICY) and box.exists(UNIT_FILE), "%s: no policy or unit file" % op)
            elif op == "stop":
                box.wait_until(lambda: box.active() != "active", timeout=30, what="the service stopped")
            elif op == "uninstall":
                expect(box.prop("LoadState") == "not-found", "uninstall: the unit is still known")
                for p in (INSTALLED, UNIT_FILE, POLICY):
                    expect(not box.exists(p), "uninstall: %s is still there" % p)
                expect(box.exists(DATA + "/config.json"), "uninstall: the settings were removed without being asked")
                if n == LEFT_BEHIND_AT:
                    # What an uninstall that removed the unit and then failed on the program leaves: the
                    # window's next uninstall must finish it, through the staged helper, with no unit to disable.
                    box.sh("install -o root -g root -m 755 %s %s" % (PROGRAM, INSTALLED))
            expect_root_clean(box, "after %s" % op, must_exist=op != "uninstall")
            held = held_files(box, window_pid(box))
            expect(not held, "INV-128: after %s the window holds %r" % (op, held))
            box.sh("runuser -u tuser -- touch %s" % CONTINUE)   # the window removes it and goes on
            at = i + 1
        w.wait_line("asb-gui-ops done", at, timeout=60)
    finally:
        w.stop()
