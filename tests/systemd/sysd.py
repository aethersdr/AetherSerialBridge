"""The systemd suite's container harness (WS-8, master 8.1).

Each case gets its own debian:bookworm container with systemd as PID 1 (privileged, a private cgroup
namespace, tmpfs /run and /tmp, exactly as the containers CI leg boots it), the dev build mounted read-only
at /usr/lib/asb-test/bin, the Qt tree at the path the build configured (RUNPATH, no LD_LIBRARY_PATH), and a host
ext4 directory bind-mounted at /var/lib/aether-serial-bridge (INV-116: the lock directory may not be
overlayfs). A "reboot" is `docker restart` (R9: persistence only; real boot ordering is HW 8).

Two modes (WS-13):
  dev      (default) the binary under test is a test build; it is also the seam build (SEAM_DIR = the same files).
  release  the binary under test (BIN) is the RELEASE binary: no test seams, static Qt, no Qt mount. A seam build
           of the same commit and Qt is mounted at SEAM_DIR. Every operation of the product runs the release
           bytes: install, uninstall and apply-without-a-config through the real CLI (outcome from the exit code),
           every other Applier operation through the real `--helper` request (the Linux window's path, which answers
           with the full Result). Only what a release cannot do comes from the seam build: an operation that a
           case runs with ASB_TEST_* (a failure point), an operation paused mid-way (op_paused), and the probes
           (hold/try a lock, the seccomp probe, tst_HelperRoundTrip). Cases that test the seam build itself and
           involve no release bytes are registered with release=False and skipped in this mode.

Nothing here runs systemctl on the host: systemctl, busctl, strace and inotifywait run INSIDE the container,
as observations. The product itself talks to systemd only over D-Bus.
"""

import base64
import json
import os
import re
import shlex
import shutil
import subprocess
import time

BIN = "/usr/lib/asb-test/bin"
EXE = BIN + "/aether-serial-bridge"
SEAM_DIR = "/usr/lib/asb-test/seam"
SEAM = SEAM_DIR + "/aether-serial-bridge"
PROBE = SEAM_DIR + "/asb_seccomp_probe"
RELEASE = False   # set by driver.py --release
DIST = "/usr/lib/asb-test/dist"   # driver.py --dist: the release artifacts (the AppImage case)
FIXTURES = "/usr/lib/asb-test/fixtures"   # driver.py --fixtures: pinned program fixtures (tests/systemd/fixtures.env)
GUI_DIR = "/usr/lib/asb-test/gui"   # driver.py --gui-bin (v1.1 WS-17): the seam window and the program beside it
INSTALLED = "/usr/local/bin/aether-serial-bridge"
UNIT = "aether-serial-bridge.service"
DATA = "/etc/aether-serial-bridge"
LOCKDIR = "/var/lib/aether-serial-bridge"
RUNDIR = "/run/aether-serial-bridge"


class CaseFail(Exception):
    pass


class CaseSkip(Exception):
    pass


def expect(cond, msg):
    if not cond:
        raise CaseFail(msg)


class Result:
    def __init__(self, rc, out, err):
        self.rc, self.out, self.err = rc, out, err

    def __repr__(self):
        return "rc=%s out=%r err=%r" % (self.rc, self.out[-2000:], self.err[-2000:])


class Box:
    def __init__(self, image, bindir, qt_host, qt_version, workdir, name, lockdir_mount=True, seam_dir=None, extra_mounts=()):
        self.image, self.bindir, self.qt_host, self.qt_version = image, bindir, qt_host, qt_version
        self.seam_dir = seam_dir or bindir
        self.extra_mounts = list(extra_mounts)
        self.name = name
        self.hostlock = os.path.join(workdir, name)
        self.lockdir_mount = lockdir_mount
        self.started = False

    # --- lifecycle ---------------------------------------------------------------------------------
    def boot(self):
        os.makedirs(self.hostlock, exist_ok=True)
        # Labelled (org.aethersdr.asb-ci: everything CI starts carries it) and capped, like every container of the
        # CI containers leg.
        cmd = ["docker", "run", "-d", "--name", self.name, "--privileged", "--cgroupns=private",
               "--label", "org.aethersdr.asb-ci=containers", "--memory", "2g", "--cpus", "2",
               "--tmpfs", "/run", "--tmpfs", "/run/lock", "--tmpfs", "/tmp",
               "-v", "%s:%s:ro" % (self.bindir, BIN),
               "-v", "%s:%s:ro" % (self.seam_dir, SEAM_DIR)]
        if self.qt_host:   # a dynamic-Qt build finds Qt at the path it was configured with (RUNPATH)
            cmd += ["-v", "%s:/qt/%s/gcc_64:ro" % (self.qt_host, self.qt_version)]
        for host, guest in self.extra_mounts:
            cmd += ["-v", "%s:%s:ro" % (host, guest)]
        if self.lockdir_mount:
            cmd += ["-v", "%s:%s" % (self.hostlock, LOCKDIR)]
        cmd += [self.image]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
        self.started = True
        self.wait_running()
        if self.lockdir_mount:
            # The host directory arrives with the host's owner and any setgid bit: make it what install
            # would have created (root:root 0755).
            self.sh("chown root:root %s && chmod 0755 %s && chmod g-s %s" % (LOCKDIR, LOCKDIR, LOCKDIR))
        self.sh("useradd -m -u 1000 tuser >/dev/null 2>&1 || true")

    def wait_running(self, timeout=90):
        end = time.time() + timeout
        state = ""
        while time.time() < end:
            r = self.run("systemctl is-system-running", timeout=20)
            state = r.out.strip()
            if state in ("running", "degraded"):
                return
            time.sleep(0.5)
        raise CaseFail("systemd did not boot (state %r)" % state)

    def reboot(self):
        """docker restart: tmpfs /run is fresh, the container's file system and the lock directory persist."""
        subprocess.run(["docker", "restart", "-t", "15", self.name], check=True, stdout=subprocess.DEVNULL)
        self.wait_running()

    def destroy(self):
        if not self.started:
            return
        try:
            # Root-owned files in the host bind mount: removed from inside, while the container still exists.
            subprocess.run(["docker", "exec", self.name, "sh", "-c", "rm -rf %s/* %s/.[!.]* 2>/dev/null; true" % (LOCKDIR, LOCKDIR)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60)
        except Exception:
            pass
        subprocess.run(["docker", "rm", "-fv", self.name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        shutil.rmtree(self.hostlock, ignore_errors=True)

    # --- commands -----------------------------------------------------------------------------------
    def run(self, cmd, user=None, env=None, timeout=180):
        argv = ["docker", "exec", "-e", "LANG=C.UTF-8"]
        if user:
            argv += ["-u", user]
        for k, v in (env or {}).items():
            argv += ["-e", "%s=%s" % (k, v)]
        argv += [self.name, "bash", "-c", cmd]
        try:
            p = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        except subprocess.TimeoutExpired:
            raise CaseFail("timed out after %ss: %s" % (timeout, cmd))
        return Result(p.returncode, p.stdout.decode(errors="replace"), p.stderr.decode(errors="replace"))

    def sh(self, cmd, **kw):
        """run(), and fail the case unless it exits 0."""
        r = self.run(cmd, **kw)
        expect(r.rc == 0, "command failed: %s -> %r" % (cmd, r))
        return r.out

    def bg(self, cmd, env=None):
        argv = ["docker", "exec", "-e", "LANG=C.UTF-8"]
        for k, v in (env or {}).items():
            argv += ["-e", "%s=%s" % (k, v)]
        argv += [self.name, "bash", "-c", cmd]
        return subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    # --- the Applier: through the real CLI where it has the command (WS-9), else --test-op ---------------
    # The CLI's operations run on the same Platform with the same seams in a test build (ASB_TEST_PAUSE,
    # ASB_TEST_FAIL); ASB_TEST_RESULT_JSON=1 makes it print the final Result as the JSON line --test-op prints.
    CLI_OPS = {"install": ["--install"], "start": ["--start"], "stop": ["--stop"], "repair": ["--repair", "--yes"],
               "uninstall": ["--uninstall"], "discard-pending": ["--discard-pending"]}

    def _command(self, name, args):
        args = list(args)
        # The CLI's commands take no --config (INV-26): an operation given one (an untick with a save) is --test-op's.
        if (name in self.CLI_OPS and "--config" not in args) or (name == "apply" and "--config" not in args):
            argv = (self.CLI_OPS.get(name) or ["--apply"]) + args
            return "%s %s" % (EXE, " ".join(shlex.quote(a) for a in argv)), {"ASB_TEST_RESULT_JSON": "1"}
        return "%s --test-op %s %s" % (EXE, name, " ".join(shlex.quote(a) for a in args)), {}

    # Release mode: the CLI's commands without a Result line, and the helper's operations (a JSON request on stdin).
    RELEASE_CLI = {"install", "uninstall"}
    HELPER_OPS = {"apply": "save-and-apply", "deactivate": "deactivate", "start": "start", "stop": "stop",
                  "repair": "repair", "discard-pending": "discard-pending"}

    def _release_op(self, name, args, env, timeout):
        args = list(args)
        if name in self.RELEASE_CLI or (name == "apply" and "--config" not in args):
            argv = (self.CLI_OPS.get(name) or ["--apply"]) + args
            r = self.run("%s %s" % (EXE, " ".join(shlex.quote(a) for a in argv)), env=env, timeout=timeout)
            msg = (r.out + r.err).strip()
            # The CLI prints the Result's message and exits 0 or 1; NothingChanged is the one outcome whose message
            # has one fixed opening (Applier's nothing()).
            outcome = "Ok" if r.rc == 0 else (
                "NothingChanged" if msg.startswith("The settings and the service were not changed") else "Failed(exit %d)" % r.rc)
            res = {"outcome": outcome, "message": msg, "via": "cli"}
            res["_rc"], res["_err"], res["_out"] = r.rc, r.err, r.out
            return res
        req = {"op": self.HELPER_OPS[name]}
        if "--config" in args:
            data = self.read(args[args.index("--config") + 1])
            expect(data is not None, "%s: the config file to send is missing" % name)
            req["configBytes"] = base64.b64encode(data.encode()).decode()
            req["source"] = {"kind": "new"}
            req["supersedePending"] = True
        self.write("/tmp/release-op-request", json.dumps(req))
        # The installed program's helper, as the window runs it (pkexec /usr/local/bin/aether-serial-bridge --helper):
        # the release bytes whenever the release is installed. After a failure point of the seam build's install it is
        # that build, which counts its own installation as Current (v1.1 INV-120: the release binary's helper would
        # see a different build there and refuse, which is the product's rule, not the case's subject).
        program = INSTALLED if self.run("test -f %s" % INSTALLED).rc == 0 else EXE
        r = self.run("%s --helper < /tmp/release-op-request" % program, env=env, timeout=timeout)
        try:
            res = json.loads(r.out)
        except ValueError:
            res = {"outcome": "<no result>", "message": r.out + r.err}
        res["via"] = "helper"
        res["_rc"], res["_err"], res["_out"] = r.rc, r.err, r.out
        return res

    def _seam_command(self, name, args):
        # v1.1 INV-120: the seam build counts the installed program as its own build (ASB_TEST_PROGRAM_SHA256, a test
        # seam), so its failure points and paused operations see the RELEASE installation as the release does; with
        # nothing installed, it falls back to its own image.
        return "ASB_TEST_PROGRAM_SHA256=$(sha256sum %s 2>/dev/null | cut -c1-64) %s --test-op %s %s" % (
            INSTALLED, SEAM, name, " ".join(shlex.quote(a) for a in args))

    def op(self, name, *args, env=None, timeout=240):
        e = dict(env or {})
        if RELEASE:
            # A failure point (ASB_TEST_*) or an operation the release has no route for comes from the seam build.
            seam = any(k.startswith("ASB_TEST") for k in e) or not (
                name in self.RELEASE_CLI or name in self.HELPER_OPS) or (name == "apply" and "--force-recreate" in args)
            if not seam:
                return self._release_op(name, args, e, timeout)
            cmd, extra = self._seam_command(name, args), {}
        else:
            cmd, extra = self._command(name, args)
        e.update(extra)
        r = self.run(cmd, env=e, timeout=timeout)
        res = None
        for line in r.out.splitlines():
            line = line.strip()
            if line.startswith("{"):
                try:
                    res = json.loads(line)
                except ValueError:
                    pass
        if res is None:
            res = {"outcome": "<no result>", "message": r.out + r.err}
        res["_rc"], res["_err"], res["_out"] = r.rc, r.err, r.out
        return res

    def op_paused(self, name, args=(), pause="", env=None, timeout=120):
        """Starts an operation that pauses at `pause` (ASB_TEST_PAUSE); returns once it says so. Release mode: the
        seam build (the release has no pause points)."""
        cmd, extra = (self._seam_command(name, args), {}) if RELEASE else self._command(name, args)
        e = dict(env or {})
        e.update(extra)
        e["ASB_TEST_PAUSE"] = pause
        p = self.bg(cmd, env=e)
        end = time.time() + timeout
        err = b""
        os.set_blocking(p.stderr.fileno(), False)
        while time.time() < end:
            chunk = p.stderr.read() or b""
            err += chunk
            if b"ASB-PAUSED" in err:
                return p
            if p.poll() is not None:
                rest = p.stderr.read() or b""
                raise CaseFail("the operation ended before pausing at %s: %r" % (pause, (err + rest).decode(errors="replace")[-2000:]))
            time.sleep(0.1)
        p.kill()
        raise CaseFail("the operation did not pause at %s" % pause)

    def kill_ops(self):
        """kill -9 every Applier process in the container (the crash of an Applier): --test-op and the CLI's."""
        self.run("pkill -9 -f -- '--test-op|aether-serial-bridge --(install|apply|start|stop|repair|uninstall|discard-pending)'"
                 " ; sleep 0.3; true")

    def cli(self, *args, user=None, env=None, timeout=240):
        """The real CLI, as a user would run it (no JSON)."""
        return self.run("%s %s" % (EXE, " ".join(shlex.quote(a) for a in args)), user=user, env=env, timeout=timeout)

    def apply(self, config, *extra, env=None):
        self.write("/tmp/apply.json", json.dumps(config))
        return self.op("apply", "--config", "/tmp/apply.json", *extra, env=env)

    def install(self, *extra, env=None):
        return self.op("install", *extra, env=env)

    # --- observations ------------------------------------------------------------------------------
    def write(self, path, text, mode=None):
        r = subprocess.run(["docker", "exec", "-i", self.name, "sh", "-c", "cat > %s" % shlex.quote(path)],
                           input=text.encode(), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        expect(r.returncode == 0, "cannot write %s: %r" % (path, r.stderr))
        if mode:
            self.sh("chmod %s %s" % (mode, shlex.quote(path)))

    def read(self, path):
        r = self.run("cat %s" % shlex.quote(path))
        return r.out if r.rc == 0 else None

    def exists(self, path):
        return self.run("test -e %s || test -L %s" % (shlex.quote(path), shlex.quote(path))).rc == 0

    def prop(self, prop, unit=UNIT):
        return self.run("systemctl show -p %s --value %s" % (prop, unit)).out.strip()

    def active(self):
        return self.run("systemctl is-active %s" % UNIT).out.strip()

    def main_pid(self):
        try:
            return int(self.prop("MainPID") or "0")
        except ValueError:
            return 0

    def wait_until(self, fn, timeout=30, what="condition"):
        end = time.time() + timeout
        while time.time() < end:
            if fn():
                return
            time.sleep(0.25)
        raise CaseFail("timed out waiting for %s" % what)

    def status(self):
        s = self.read(RUNDIR + "/status.json")
        return json.loads(s) if s else None

    def journal(self, unit=UNIT):
        return self.run("journalctl --no-pager -o cat -u %s" % unit).out

    def dropin(self, name, text):
        """A runtime drop-in for the unit (lost at reboot), then daemon-reload."""
        self.sh("mkdir -p /run/systemd/system/%s.d" % UNIT)
        self.write("/run/systemd/system/%s.d/%s.conf" % (UNIT, name), text)
        self.sh("systemctl daemon-reload")

    def trace_service(self, calls="openat,open,creat,rename,renameat,renameat2,unlink,unlinkat"):
        """Runs the service under strace (same unit, same cgroup); the trace goes to the journal (stderr)."""
        self.dropin("trace", "[Service]\nExecStart=\nExecStart=/usr/bin/strace -f -qq -e trace=%s %s --service\n" % (calls, INSTALLED))

    def lock_ids(self):
        return self.read(LOCKDIR + "/engine.lock"), self.read(LOCKDIR + "/engine.lock.id")


CONFIG_A = {"version": 1, "ports": [{"enabled": True, "name": "a", "comPort": "/dev/ttyS0", "baudRate": 9600,
                                      "tcpPort": 4001, "bindAddress": "127.0.0.1", "allowedClients": ""}]}
CONFIG_B = {"version": 1, "ports": [{"enabled": True, "name": "b", "comPort": "/dev/ttyS1", "baudRate": 9600,
                                      "tcpPort": 4002, "bindAddress": "127.0.0.1", "allowedClients": ""}]}


# The suite's copy of tests/support/Fakes.cpp claimsStop (WS-5 round 6): a stop claimed in a result. Wording
# that says the stop is NOT proven is removed first; whatever is left must not say stopped / verified / off.
_NEGATED = re.compile(r"(could not|cannot|did not) prove (that )?(the bridge|the engine|no engine)[^.;]*|"
                      r"not proven stopped|prove the bridge stopped|the operation stopped", re.I)
_CLAIM = re.compile(r"stopped|verified|is off|no engine process|terminated", re.I)


def claims_stop(text):
    return bool(_CLAIM.search(_NEGATED.sub("", text or "")))


def no_stop_claim(res, what):
    """No part of the result words a stop: only engineClaim may, and only from the operation's evidence."""
    for field in ("message", "postcondition"):
        expect(not claims_stop(res.get(field)), "%s: the %s claims a stop without its proof: %r" % (what, field, res.get(field)))


def ok(res, what):
    expect(res.get("outcome") == "Ok", "%s: expected Ok, got %s: %s" % (what, res.get("outcome"), res.get("message")))


def running_with(box, port):
    """The service is active, and its own status says it listens on `port`."""
    def check():
        s = box.status()
        return box.active() == "active" and s is not None and any(p.get("tcpPort") == port for p in s.get("ports", []))
    box.wait_until(check, timeout=30, what="the service listening on %d" % port)


class PolkitBox(Box):
    """v1.1 WS-15: a container of ANOTHER distribution's polkit (tests/polkit/fedora.Dockerfile), without systemd: the
    system bus and polkitd are started by hand, the program under test and its Qt mounted as in Box. Not privileged
    (pkexec and polkit-agent-helper-1 are set-user-ID programs of the image)."""

    def boot(self):
        cmd = ["docker", "run", "-d", "--name", self.name, "--label", "org.aethersdr.asb-ci=containers",
               "--memory", "2g",
               "--cpus", "2", "--tmpfs", "/run", "--tmpfs", "/tmp",
               "-v", "%s:%s:ro" % (self.bindir, BIN), "-v", "%s:%s:ro" % (self.seam_dir, SEAM_DIR)]
        if self.qt_host:
            cmd += ["-v", "%s:/qt/%s/gcc_64:ro" % (self.qt_host, self.qt_version)]
        for host, guest in self.extra_mounts:
            cmd += ["-v", "%s:%s:ro" % (host, guest)]
        cmd += [self.image]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
        self.started = True
        self.sh("mkdir -p /run/dbus && dbus-daemon --system --fork")
        self.sh("(nohup /usr/lib/polkit-1/polkitd --no-debug >/run/polkitd.log 2>&1 &)")
        end = time.time() + 30
        while time.time() < end:
            if self.run("pkaction --action-id org.freedesktop.policykit.exec >/dev/null 2>&1").rc == 0:
                return
            time.sleep(0.3)
        raise CaseFail("polkitd did not answer: %r" % self.run("cat /run/polkitd.log").out[-1500:])

    def destroy(self):
        if self.started:
            subprocess.run(["docker", "rm", "-fv", self.name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# --- the case registry ----------------------------------------------------------------------------------
REGISTRY = {}
KINDS = {}   # name -> "fedora" for PolkitBox cases (driver.py --fedora-image); absent = the systemd Box


def case(name, lockdir=True, release=True, kind=None):
    """Registers a case: fn(box). `lockdir=False` boots without the ext4 lock-directory bind mount. `release=False`:
    the case exercises the seam build only (probes, service-side seams) and is skipped in release mode. `kind="fedora"`:
    the case runs in a PolkitBox of the Fedora polkit image instead of the systemd container."""
    def wrap(fn):
        REGISTRY[name] = (fn, lockdir, release)
        if kind:
            KINDS[name] = kind
        return fn
    return wrap
