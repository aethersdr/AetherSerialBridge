"""--service activation identity (INV-100, N1), the process guard under PID 1 (INV-110), port listing
(INV-32)."""

import time

from sysd import (CONFIG_A, DATA, EXE, INSTALLED, LOCKDIR, PROBE, UNIT, CaseSkip, case, expect, ok, running_with)

TRACE = "strace -f -qq -e trace=open,openat,openat2,creat -o /tmp/svc.trace"


def masked_install(box):
    ok(box.install(), "install")
    ok(box.apply(CONFIG_A), "apply")
    running_with(box, 4001)
    res = box.op("deactivate")
    expect(res["outcome"] in ("Ok", "SavedNotApplied"), "deactivate: %s" % res)
    expect(box.prop("LoadState") == "masked", "not masked")


def refused_before_any_open(box, rc, trace="/tmp/svc.trace"):
    expect(rc == 2, "exit %s, expected 2" % rc)
    t = box.read(trace) or ""
    touched = [l for l in t.splitlines() if DATA in l or LOCKDIR in l or "/run/aether-serial-bridge" in l]
    expect(not touched, "opened before the identity check: %s" % touched[:5])


@case("forged_invocation_id_direct_exec_while_masked")
def forged_invocation_id(box):
    masked_install(box)
    r = box.run("INVOCATION_ID=0123456789abcdef0123456789abcdef %s %s --service" % (TRACE, INSTALLED))
    refused_before_any_open(box, r.rc)
    expect("runs only as the aether-serial-bridge system service" in r.err, r.err)


@case("unrelated_system_unit_runs_service")
def unrelated_system_unit(box):
    masked_install(box)
    r = box.run("systemd-run --wait -q --unit=asb-other -p StandardOutput=journal %s %s --service; "
                "systemctl show -p ExecMainStatus --value asb-other.service" % (TRACE, INSTALLED))
    status = box.run("journalctl --no-pager -o cat -u asb-other.service").out
    expect("runs only as the aether-serial-bridge system service" in status, status)
    expect("0::/system.slice/asb-other.service" in status, status)
    # The unit's own exit status (strace exits with its tracee's), not a literal.
    lines = r.out.strip().splitlines()
    expect(lines and lines[-1].strip().isdigit(), "no ExecMainStatus: %r" % r)
    refused_before_any_open(box, int(lines[-1].strip()))


@case("user_unit_runs_service")
def user_unit(box):
    masked_install(box)
    box.sh("systemctl start user@1000.service")
    r = box.run("runuser -u tuser -- env XDG_RUNTIME_DIR=/run/user/1000 systemd-run --user --wait --pipe -q "
                "%s -o /tmp/user.trace %s --service" % (TRACE.replace(" -o /tmp/svc.trace", ""), INSTALLED))
    expect("/user.slice/" in r.err and "runs only as" in r.err, "%r" % r)
    refused_before_any_open(box, r.rc, trace="/tmp/user.trace")   # systemd-run --wait --pipe exits with the unit's status


@case("systemd_run_scope_runs_service")
def scope(box):
    masked_install(box)
    r = box.run("systemd-run --scope -q %s %s --service" % (TRACE, INSTALLED))
    expect(".scope" in r.err, r.err)
    refused_before_any_open(box, r.rc)


@case("cgroupns_rebased_refused")
def cgroupns(box):
    """N1: in a nested cgroup namespace the path is rebased ("0::/") and refused. The unprivileged form
    (unshare -U -r -C as a normal user) is used where the kernel allows user namespaces; otherwise root's
    `unshare -C` shows the same rebased view."""
    masked_install(box)
    r = box.run("runuser -u tuser -- unshare -U -r -C cat /proc/self/cgroup")
    if r.rc == 0:
        variant = "runuser -u tuser -- unshare -U -r -C"
    else:
        variant = "unshare -C"
    r = box.run("%s %s %s --service" % (variant, TRACE, INSTALLED))
    expect('"0::/; "' in r.err or "0::/;" in r.err, "%s: %r" % (variant, r))
    refused_before_any_open(box, r.rc)
    print("   (cgroupns variant: %s)" % variant)


@case("delegated_subtree_cannot_join_unit")
def delegated(box):
    """N1: a user with a delegated cgroup subtree cannot move a process into the unit's root-owned
    cgroup.procs."""
    ok(box.install(), "install")
    box.wait_until(lambda: box.active() == "active", timeout=20, what="the service")
    box.sh("systemd-run -q --unit=deleg -p Delegate=yes -p User=tuser sleep 300")
    pid = box.sh("systemctl show -p MainPID --value deleg.service").strip()
    procs = "/sys/fs/cgroup/system.slice/%s/cgroup.procs" % UNIT
    r = box.run("runuser -u tuser -- sh -c 'echo %s > %s'" % (pid, procs))
    expect(r.rc != 0 and ("Permission denied" in r.err or "Operation not permitted" in r.err), "%r" % r)
    members = box.sh("cat %s" % procs).split()
    expect(pid not in members, "the delegated process joined the unit: %s" % members)


# --- the process guard under PID 1 (the same probe as tst_ProcessGuard, plus the system bus) -------------
def probe(box, name):
    r = box.run("%s %s" % (PROBE, name))
    return r


for _name, _case, _errno in (("applier_cannot_fork", "fork-libc", "EPERM"), ("applier_cannot_fork_syscall", "fork-raw", "EPERM"),
                             ("applier_cannot_vfork", "vfork-raw", "EPERM"), ("applier_cannot_clone_process", "clone-process", "EPERM"),
                             ("applier_clone3_enosys", "clone3", "ENOSYS"), ("applier_cannot_exec", "execve", "EPERM"),
                             ("applier_cannot_execveat", "execveat", "EPERM")):
    def _make(c=_case, e=_errno):
        def fn(box):
            r = probe(box, c)
            if r.out.startswith("SKIP"):
                raise CaseSkip(r.out.strip())
            expect(r.rc == 0 and ("DENIED %s %s" % (c, e)) in r.out, "%r" % r)
        return fn
    case(_name)(_make())


@case("i386_int80_killed", release=False)
def int80(box):
    r = probe(box, "int80")
    if r.out.startswith("SKIP"):
        raise CaseSkip(r.out.strip())
    expect(r.rc == 128 + 31 and "ALLOWED" not in r.out, "%r" % r)


@case("x32_syscall_killed", release=False)
def x32(box):
    r = probe(box, "x32")
    if r.out.startswith("SKIP"):
        raise CaseSkip(r.out.strip())
    expect(r.rc == 128 + 31 and "ALLOWED" not in r.out, "%r" % r)


@case("threads_still_work_under_filter", release=False)
def threads(box):
    r = probe(box, "threads")
    expect(r.rc == 0 and "OK threads 11" in r.out, "%r" % r)


@case("guard_installs_as_root_without_cap_sys_admin", release=False)
def root_without_sys_admin(box):
    """INV-110 (#13: the CI Docker runner's root has no CAP_SYS_ADMIN): uid 0 is not CAP_SYS_ADMIN, so the
    guard sets no_new_privs for root too; without it seccomp() is EPERM and every root process without the
    capability would exit 5. Root here runs with CAP_SYS_ADMIN dropped from its bounding set."""
    r = box.run("setpriv --bounding-set -sys_admin --inh-caps -sys_admin %s fork-raw" % PROBE)
    expect(r.rc == 0 and "DENIED" in r.out, "the guard did not install as root without CAP_SYS_ADMIN: %r" % r)
    st = box.run("setpriv --bounding-set -sys_admin --inh-caps -sys_admin grep -E '^CapEff' /proc/self/status").out
    expect(st.strip() and not (int(st.split()[-1], 16) >> 21) & 1, "CAP_SYS_ADMIN was not dropped: %r" % st)


@case("helper_dbus_works_under_filter", release=False)
def dbus(box):
    r = probe(box, "dbus")
    expect(r.rc == 0 and "OK dbus systemd1=1" in r.out, "%r" % r)


@case("tsync_failure_fatal", release=False)
def tsync(box):
    r = probe(box, "tsync-fake")
    expect(r.rc == 5 and "ALLOWED" not in r.out, "%r" % r)


@case("filter_install_failure_exits_5", release=False)
def install_failure(box):
    r = probe(box, "install-failure")
    expect(r.rc == 5 and "ALLOWED" not in r.out, "%r" % r)


@case("list_ports_strace")
def list_ports(box):
    """INV-32: --list-ports lists a pty-backed adapter by its stable and kernel names without opening it, its
    pts path or any /proc alias of it (strace)."""
    w = box.bg("socat PTY,link=/dev/ttyUSB7,raw,echo=0 PTY,link=/tmp/peer,raw,echo=0")
    box.wait_until(lambda: box.exists("/dev/ttyUSB7"), timeout=10, what="the pty")
    box.sh("mkdir -p /dev/serial/by-id && ln -sf ../../ttyUSB7 /dev/serial/by-id/usb-Test_Adapter-if00-port0")
    pts = box.sh("readlink -f /dev/ttyUSB7").strip()
    r = box.run("strace -f -qq -e trace=open,openat,openat2,creat -o /tmp/lp.trace %s --list-ports" % EXE)
    expect(r.rc == 0, "%r" % r)
    # The final target of the chain (by-id -> ttyUSB7 -> the pts), named without opening any of them.
    expect("/dev/serial/by-id/usb-Test_Adapter-if00-port0 -> %s" % pts in r.out, r.out)
    expect("  /dev/ttyUSB7" in r.out, r.out)
    t = box.read("/tmp/lp.trace")
    bad = [l for l in t.splitlines() if "ttyUSB7" in l or "usb-Test_Adapter" in l or pts in l or "/fd/" in l]
    expect(not bad, "a device path was opened: %s" % bad)
    w.kill()
