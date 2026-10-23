"""The CLI as an operator runs it (WS-9): the whole station cycle with real commands, root required, the
read-only views as an unprivileged user (INV-44 native), and --run's isolation (INV-90, INV-101) against the
installed program."""

import json
import os
import time

from sysd import DATA, EXE, INSTALLED, RUNDIR, UNIT, case, expect, running_with


_LISTENERS = r"""
import socket, struct, sys
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


def listeners(box, port):
    """The local addresses the kernel lists as LISTEN on `port` (the OS table, not the program's word)."""
    box.write("/tmp/listeners.py", _LISTENERS)
    return box.run("python3 /tmp/listeners.py %d" % port).out.split()


def _ok(r, what):
    expect(r.rc == 0, "%s: rc=%s out=%r err=%r" % (what, r.rc, r.out[-1500:], r.err[-1500:]))


@case("cli_station_cycle")
def station_cycle(box):
    """--install, --add-port, --show-config, --apply, --status, --stop, --start, --uninstall: the commands an
    operator types, each with its exit code and its words."""
    r = box.cli("--install")
    _ok(r, "--install")
    expect(box.active() == "active" and box.exists(INSTALLED), "not installed and running after --install")
    r = box.cli("--add-port", "KAT500", "/dev/ttyS0", "4011", "--baud", "38400", "--bind", "127.0.0.1")
    _ok(r, "--add-port")
    expect("Added KAT500: /dev/ttyS0 at 38400 8N1 on 127.0.0.1:4011." in r.out and "--apply" in r.out, r.out)
    expect(box.exists(DATA + "/config.pending.json"), "the edit is not in config.pending.json")
    r = box.cli("--show-config")
    _ok(r, "--show-config")
    expect("Saved, not applied (config.pending.json)" in r.out and "KAT500" in r.out, r.out)
    r = box.cli("--apply")
    _ok(r, "--apply")
    running_with(box, 4011)
    expect(not box.exists(DATA + "/config.pending.json"), "pending survived the apply")
    r = box.cli("--status")
    _ok(r, "--status")
    expect("Service: running (pid %d)" % box.main_pid() in r.out, r.out)
    expect("KAT500" in r.out and "127.0.0.1:4011" in r.out, r.out)
    expect("Config: the bridge reported loading the saved config.json." in r.out, r.out)
    r = box.cli("--stop")
    _ok(r, "--stop")
    box.wait_until(lambda: box.active() in ("inactive", "failed"), timeout=30, what="the service to stop")
    r = box.cli("--status")
    expect("Activation: none" in r.out, "after --stop the token is gone: %s" % r.out)
    r = box.cli("--start")
    _ok(r, "--start")
    running_with(box, 4011)
    r = box.cli("--uninstall")
    _ok(r, "--uninstall")
    expect("The settings are kept in /etc/aether-serial-bridge." in r.out, r.out)
    expect(not box.exists(INSTALLED), "the installed program survived --uninstall")
    expect(not box.exists("/usr/local/lib/systemd/system/" + UNIT), "the unit survived --uninstall")
    expect(box.exists(DATA + "/config.json"), "the config was not kept")
    r = box.cli("--status")
    _ok(r, "--status after --uninstall")
    expect("Service: not installed" in r.out, r.out)


@case("cli_requires_root")
def requires_root(box):
    """A mutating command as a user is refused before anything exists: exit 1, the sudo line, nothing created."""
    for args in (["--install"], ["--apply"], ["--add-port", "K", "/dev/ttyS0", "4012"], ["--repair", "--yes"]):
        r = box.cli(*args, user="tuser")
        expect(r.rc == 1, "%s as a user: rc=%s err=%r" % (args, r.rc, r.err))
        expect("must run as root: sudo aether-serial-bridge %s" % " ".join(args) in r.err, r.err)
    expect(not box.exists(DATA) and not box.exists(INSTALLED), "something was created")
    r = box.cli("--install", "--apply")
    expect(r.rc == 2 and "cannot be combined" in r.err, "conflict: %r" % r)
    r = box.cli("--apply", "--config", "/tmp/x.json")
    expect(r.rc == 2 and "--config cannot be used with --apply" in r.err, "--config with --apply: %r" % r)


@case("show_config_unprivileged_leaves_bytes")
def show_config_unprivileged(box):
    """INV-44 (WS-5 deferred, native): --show-config and --status as an unprivileged user read the managed folder
    and change nothing: owner, mode, inode and ctime of the folder and every file are identical afterwards."""
    _ok(box.cli("--install"), "--install")
    _ok(box.cli("--add-port", "KAT500", "/dev/ttyS0", "4013", "--bind", "127.0.0.1"), "--add-port")
    stat = "stat -c '%%n %%u:%%g %%a %%i %%Z %%s' %s %s/*" % (DATA, DATA)
    before = box.sh(stat)
    time.sleep(1.1)   # a write would move ctime by at least a second
    r = box.cli("--show-config", user="tuser")
    _ok(r, "--show-config as tuser")
    expect("KAT500" in r.out and "Saved, not applied" in r.out, r.out)
    r = box.cli("--status", user="tuser")
    _ok(r, "--status as tuser")
    expect("Service: " in r.out, r.out)
    expect(box.sh(stat) == before, "the folder changed:\n%s\n---\n%s" % (before, box.sh(stat)))


@case("run_refuses_installed_image")
def run_installed_image(box):
    """INV-101: --run never runs from the installed service program — a hard link to it is the same file and is
    refused; a copy runs, on loopback only (INV-90: never the managed config, never its firewall)."""
    _ok(box.cli("--install"), "--install")
    box.sh("mkdir -p /root/scratch && chmod 700 /root/scratch")
    box.write("/root/scratch/c.json", json.dumps({"version": 1, "ports": [
        {"enabled": True, "name": "scratch", "comPort": "/dev/ttyS1", "baudRate": 9600, "tcpPort": 4014,
         "bindAddress": "0.0.0.0", "allowedClients": ""}]}))
    box.sh("ln %s /usr/local/bin/asb-hardlink" % INSTALLED)
    r = box.run("/usr/local/bin/asb-hardlink --run --config /root/scratch/c.json")
    expect(r.rc == 2 and "installed service program" in r.err, "hard link: %r" % r)
    r = box.run("%s --run --config /root/scratch/c.json" % INSTALLED)
    expect(r.rc == 2 and "installed service program" in r.err, "installed program: %r" % r)
    r = box.run("%s --run --config %s/config.json" % (EXE, DATA))
    expect(r.rc == 2 and ("inside the data folder" in r.err or "installed config.json" in r.err), "managed config: %r" % r)
    # A copy runs, and listens on 127.0.0.1 only although the config says 0.0.0.0.
    box.sh("cp %s /usr/local/bin/asb-copy" % INSTALLED)
    p = box.bg("/usr/local/bin/asb-copy --run --config /root/scratch/c.json")
    try:
        box.wait_until(lambda: listeners(box, 4014), timeout=20, what="the scratch listener")
        bound = listeners(box, 4014)
        expect(bound == ["127.0.0.1"], "scratch listeners on 4014: %s" % bound)
        r = box.cli("--status", "--config", "/root/scratch/c.json")
        expect("scratch" in r.out, "status beside the scratch config: %r" % r)
    finally:
        box.run("pkill -INT -f 'asb-copy --run'")
    out, err = p.communicate(timeout=30)
    expect(p.returncode == 0, "the scratch run did not end cleanly on SIGINT: rc=%s %r" % (p.returncode, err[-1500:]))
    expect(b"listens on 127.0.0.1 only" in err, err[-1500:])
    expect(box.active() == "active", "the service was disturbed by the scratch run")


@case("scratch_root_owned_config_trusted")
def scratch_root_owned(box):
    """Scratch trust is root + the invoking user (master 4.4): a readable root-owned scratch config in the
    user's own folder is trusted for --show-config and --run (not exit 3); a group-writable one is not."""
    box.sh("mkdir -p /home/tuser/scratch && chown tuser:tuser /home/tuser/scratch && chmod 700 /home/tuser/scratch")
    box.write("/home/tuser/scratch/c.json", json.dumps({"version": 1, "ports": [
        {"enabled": True, "name": "rootcfg", "comPort": "/dev/ttyS1", "baudRate": 9600, "tcpPort": 4015,
         "bindAddress": "127.0.0.1", "allowedClients": ""}]}))
    box.sh("chown root:root /home/tuser/scratch/c.json && chmod 644 /home/tuser/scratch/c.json")
    r = box.cli("--show-config", "--config", "/home/tuser/scratch/c.json", user="tuser")
    _ok(r, "--show-config of a root-owned scratch config as the user")
    expect("rootcfg" in r.out, r.out)
    box.sh("cp %s /home/tuser/asb-copy && chown tuser:tuser /home/tuser/asb-copy" % EXE)
    p = box.bg("runuser -u tuser -- /home/tuser/asb-copy --run --config /home/tuser/scratch/c.json")
    try:
        box.wait_until(lambda: listeners(box, 4015), timeout=20, what="the scratch listener")
    finally:
        box.run("pkill -INT -f 'asb-copy --run'")
    p.communicate(timeout=30)
    expect(p.returncode == 0, "the user's run of a root-owned scratch config did not end cleanly: rc=%s" % p.returncode)
    box.sh("chmod 664 /home/tuser/scratch/c.json")
    r = box.cli("--show-config", "--config", "/home/tuser/scratch/c.json", user="tuser")
    expect(r.rc == 3, "a group-writable scratch config must be refused with exit 3: %r" % r)
