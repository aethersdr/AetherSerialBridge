"""activation_matrix, Linux half (INV-99; cases owned by WS-5, run on the release binary by WS-13).

Config A: port 4001 -> /dev/ttyS0, allow 10.0.0.5. Config B: port 4001 -> /dev/ttyS1, allow 0.0.0.0/0. Each row
installs and applies A, produces one point, then makes every attempt:
  (3) `--run --config <copy of B>` from the INSTALLED program: refused, before any open of the data folder, the lock
      directory or the run directory;
  (2) the same from a non-installed copy: the listener is on 127.0.0.1 only and a connection to the container's
      own non-loopback address fails;
  (4) `--service` with a forged INVOCATION_ID by direct exec, from an unrelated system unit, from a user unit and
      under `systemd-run --scope`: refused before any of those opens;
  untick/remove rows: config.json is A's bytes and config.pending.json B's;
  (1) `systemctl start` (the unit unmasked first, as a hand would) and then a reboot: the service either does not
      run, or it runs config.json and the row allows that config to run (only after_r9, where B was synced and
      committed). The ACK digest is compared with config.json's bytes.
Rows (points):
  rename_fail    an apply of B whose sync succeeded and whose rename over config.json failed (seam build:
                 ASB_TEST_FAIL=sys:renameat:config.json);
  remove_fails   untick/remove: SaveAndRemove of B whose removal failed (seam build: ASB_TEST_FAIL=firewall:removeAll);
  deactivate     untick: Deactivate with B (the release binary's own --helper path in release mode);
  after_r9       B applied to the end: committed, synced and started (the state a crash after R9 leaves).
In release mode (driver.py --release) the installed program, every attempt and every operation without a failure
point are the release bytes; only the two failure points come from the seam build of the same commit."""

import hashlib
import json
import time

from sysd import (DATA, EXE, INSTALLED, LOCKDIR, RUNDIR, UNIT, case, expect, ok)

TRACE = "strace -f -qq -e trace=open,openat,openat2,creat"

A = {"version": 1, "ports": [{"enabled": True, "name": "mx", "comPort": "/dev/ttyS0", "baudRate": 9600, "tcpPort": 4001,
                              "bindAddress": "0.0.0.0", "allowedClients": "10.0.0.5"}]}
B = {"version": 1, "ports": [{"enabled": True, "name": "mx", "comPort": "/dev/ttyS1", "baudRate": 9600, "tcpPort": 4001,
                              "bindAddress": "0.0.0.0", "allowedClients": "0.0.0.0/0"}]}

_LISTENERS = r"""
import socket, sys
port = int(sys.argv[1])
for table, fam in (("/proc/net/tcp", 4), ("/proc/net/tcp6", 6)):
    try:
        lines = open(table).read().splitlines()[1:]
    except OSError:
        continue
    for l in lines:
        c = l.split()
        if c[3] != "0A":
            continue
        addr, p = c[1].split(":")
        if int(p, 16) != port:
            continue
        raw = bytes.fromhex(addr)
        if fam == 4:
            print(socket.inet_ntop(socket.AF_INET, raw[::-1]))
        else:
            print(socket.inet_ntop(socket.AF_INET6, b"".join(raw[i:i + 4][::-1] for i in range(0, 16, 4))))
"""

_CONNECT = r"""
import socket, sys
s = socket.socket()
s.settimeout(3)
try:
    s.connect((sys.argv[1], int(sys.argv[2])))
    print("connected")
except OSError as e:
    print("refused %s" % e)
"""


def _sha(text):
    return hashlib.sha256(text.encode() if isinstance(text, str) else text).hexdigest()


def _untouched(box, trace, what):
    t = box.read(trace) or ""
    touched = [l for l in t.splitlines() if DATA in l or LOCKDIR in l or RUNDIR in l]
    expect(not touched, "%s opened before refusing: %s" % (what, touched[:5]))


def _attempt_run_installed(box):
    r = box.run("%s -o /tmp/run-inst.trace %s --run --config /root/scratch/b.json" % (TRACE, INSTALLED))
    expect(r.rc == 2 and "installed service program" in r.err, "(3) the installed program's --run: %r" % r)
    _untouched(box, "/tmp/run-inst.trace", "(3) the installed program's --run")


def _attempt_run_copy(box):
    if box.active() == "active":
        box.run("systemctl stop %s" % UNIT)
    box.sh("cp %s /usr/local/sbin/asb-copy" % EXE)
    box.write("/tmp/listeners.py", _LISTENERS)
    box.write("/tmp/connect.py", _CONNECT)
    p = box.bg("/usr/local/sbin/asb-copy --run --config /root/scratch/b.json")
    try:
        box.wait_until(lambda: box.run("python3 /tmp/listeners.py 4001").out.split(), timeout=20, what="(2) the scratch listener")
        bound = box.run("python3 /tmp/listeners.py 4001").out.split()
        expect(bound == ["127.0.0.1"], "(2) the scratch run listens on %s, not 127.0.0.1 only" % bound)
        ip = box.sh("hostname -I").split()[0]
        r = box.run("python3 /tmp/connect.py %s 4001" % ip)
        expect(r.out.startswith("refused"), "(2) a connection to %s:4001 got through: %r" % (ip, r))
    finally:
        box.run("pkill -INT -f 'asb-copy --run'")
        p.communicate(timeout=30)


def _attempt_identity(box):
    forged = box.run("INVOCATION_ID=0123456789abcdef0123456789abcdef %s -o /tmp/forged.trace %s --service" % (TRACE, INSTALLED))
    expect(forged.rc == 2 and "runs only as the aether-serial-bridge system service" in forged.err, "(4) forged: %r" % forged)
    _untouched(box, "/tmp/forged.trace", "(4) a forged INVOCATION_ID")
    r = box.run("systemd-run --wait -q --unit=asb-matrix-other -p StandardOutput=journal %s -o /tmp/other.trace %s --service; "
                "systemctl show -p ExecMainStatus --value asb-matrix-other.service" % (TRACE, INSTALLED))
    lines = r.out.strip().splitlines()
    expect(lines and lines[-1].strip() == "2", "(4) an unrelated system unit: %r" % r)
    _untouched(box, "/tmp/other.trace", "(4) an unrelated system unit")
    box.sh("systemctl start user@1000.service")
    r = box.run("runuser -u tuser -- env XDG_RUNTIME_DIR=/run/user/1000 systemd-run --user --wait --pipe -q "
                "%s -o /tmp/user.trace %s --service" % (TRACE, INSTALLED))
    expect(r.rc == 2 and "runs only as" in r.err, "(4) a user unit: %r" % r)
    _untouched(box, "/tmp/user.trace", "(4) a user unit")
    r = box.run("systemd-run --scope -q %s -o /tmp/scope.trace %s --service" % (TRACE, INSTALLED))
    expect(r.rc == 2 and ".scope" in r.err, "(4) a scope: %r" % r)
    _untouched(box, "/tmp/scope.trace", "(4) systemd-run --scope")


def _check_running(box, may_run, how):
    """Running = the unit's main process has written its ACK (status.json with its own pid). A refused start (the
    unit is Type=simple: "active" for the moment before the refusal) never gets there."""
    ack = None
    end = time.time() + 20
    while time.time() < end:
        pid = box.main_pid()
        s = box.status()
        if pid and s and s.get("pid") == pid and s.get("configSha256"):
            ack = s["configSha256"]
            break
        if box.active() not in ("active", "activating", "reloading"):
            break
        time.sleep(0.5)
    if ack is None:
        return
    expect(may_run, "(1) %s: the service runs (pid %s, ACK %s) although no synced config may run at this point"
           % (how, box.main_pid(), ack))
    committed = box.read(DATA + "/config.json") or ""
    expect(ack == _sha(committed), "(1) %s: the service ACKs %s, config.json is %s" % (how, ack, _sha(committed)))


def _settle(box, may_run):
    """A refused start may cycle (activating -> auto-restart): give a run the time to come up, then judge."""
    end = time.time() + (20 if may_run else 12)
    while time.time() < end and box.active() != "active":
        time.sleep(0.5)


def _attempt_start_and_reboot(box, may_run):
    box.run("systemctl unmask %s; systemctl start --no-block %s" % (UNIT, UNIT))
    _settle(box, may_run)
    _check_running(box, may_run, "systemctl start")
    box.reboot()
    _settle(box, may_run)
    _check_running(box, may_run, "a reboot")
    if may_run:
        s = box.status()
        expect(box.active() == "active" and s and s.get("configSha256"), "(1) the committed, synced B does not run after a reboot")


def _row(point):
    def fn(box):
        ok(box.install(), "install")
        ok(box.apply(A), "apply A")
        a_bytes = box.read(DATA + "/config.json")
        expect(a_bytes and json.loads(a_bytes)["ports"][0]["comPort"] == "/dev/ttyS0", "config.json is not A")
        b_text = json.dumps(B)
        box.write("/tmp/b.json", b_text)
        box.sh("mkdir -p /root/scratch && chmod 700 /root/scratch")
        box.write("/root/scratch/b.json", b_text)
        untick, may_run = False, False
        if point == "rename_fail":
            res = box.op("apply", "--config", "/tmp/b.json", env={"ASB_TEST_FAIL": "sys:renameat:config.json"})
            expect(res.get("outcome") != "Ok", "the apply succeeded although the rename failed: %s" % res)
            expect(box.read(DATA + "/config.json") == a_bytes, "config.json changed although its rename failed")
        elif point == "remove_fails":
            res = box.op("uninstall", "--config", "/tmp/b.json", env={"ASB_TEST_FAIL": "firewall:removeAll"})
            expect(res.get("outcome") != "Ok", "SaveAndRemove succeeded although the removal failed: %s" % res)
            untick = True
        elif point == "deactivate":
            res = box.op("deactivate", "--config", "/tmp/b.json")
            expect(res.get("outcome") in ("Ok", "SavedNotApplied"), "deactivate: %s" % res)
            untick = True
        else:   # after_r9
            ok(box.apply(B), "apply B")
            expect(json.loads(box.read(DATA + "/config.json"))["ports"][0]["comPort"] == "/dev/ttyS1", "config.json is not B")
            may_run = True
        if untick:
            expect(box.read(DATA + "/config.json") == a_bytes, "untick: committed config.json is not A")
            expect(box.read(DATA + "/config.pending.json") == b_text, "untick: config.pending.json is not B's bytes")
        if box.exists(INSTALLED):
            _attempt_run_installed(box)
            _attempt_identity(box)
        _attempt_run_copy(box)
        if untick:
            expect(box.read(DATA + "/config.json") == a_bytes, "untick: config.json changed by the attempts")
            expect(box.read(DATA + "/config.pending.json") == b_text, "untick: config.pending.json changed by the attempts")
        _attempt_start_and_reboot(box, may_run)
    return fn


for _point in ("rename_fail", "remove_fails", "deactivate", "after_r9"):
    case("activation_matrix(%s)" % _point)(_row(_point))
