"""Install, apply, start, stop, deactivate, uninstall, reboot and the fail-closed mask (INV-48, 49, 79, 96,
98, 115; WS-3 deferred INV-77; WS-5 deferred INV-73, INV-113 rows)."""

import json
import time

from sysd import (SEAM, CONFIG_A, CONFIG_B, DATA, EXE, INSTALLED, LOCKDIR, RUNDIR, UNIT, CaseFail, case, expect, ok,
                  running_with)


def installed_and_running(box, config=CONFIG_A):
    ok(box.install(), "install")
    ok(box.apply(config), "apply")
    running_with(box, config["ports"][0]["tcpPort"])


@case("mask_unmask_start_stop_over_dbus", release=False)
def mask_unmask_start_stop(box):
    """The whole cycle over D-Bus, with no process created by the Applier (strace of the operations)."""
    r = box.run("strace -f -qq -e trace=execve,execveat,fork,vfork -o /tmp/ops.trace %s --test-op install" % "/usr/lib/asb-test/bin/aether-serial-bridge")
    expect(r.rc == 0, "install: %r" % r)
    expect(box.active() == "active", "not active after install")
    expect(box.prop("UnitFileState") == "enabled", "not enabled after install")
    for op in ("stop", "start", "deactivate", "start"):
        r = box.run("strace -f -qq -e trace=execve,execveat,fork,vfork -o /tmp/ops-%s.trace /usr/lib/asb-test/bin/aether-serial-bridge --test-op %s" % (op, op))
        expect(r.rc == 0, "%s: %r" % (op, r))
    expect(box.active() == "active", "not active after the last start")
    traces = box.run("cat /tmp/ops*.trace").out
    execs = [l for l in traces.splitlines() if "execve(" in l or "execveat(" in l or "fork(" in l]
    # Only each operation's own initial execve (strace starting it) is there.
    expect(execs and all('"--test-op"' in l and l.split()[1].startswith("execve(") for l in execs) and
           len(execs) == len(set(l.split()[0] for l in execs)), "process creation by the Applier: %s" % execs)
    expect(box.run("journalctl --no-pager -o cat | grep -c 'systemctl'").out.strip() in ("0", ""), "systemctl ran")


@case("first_install_writes_empty_config")
def first_install(box):
    """INV-49: the first install commits an empty config; the service runs it (zero ports)."""
    ok(box.install(), "install")
    cfg = json.loads(box.read(DATA + "/config.json"))
    expect(cfg.get("ports") == [], "config.json after the first install: %s" % cfg)
    s = box.status()
    expect(s and s["ports"] == [] and s["configError"] is None, "status: %s" % s)
    expect(box.read(DATA + "/start.token"), "no start token")


@case("dynamic_user_writes_status")
def dynamic_user_status(box):
    """INV-96: the DynamicUser service writes status.json (Runtime policy, no chown) without EPERM; the config
    folder stays root-owned (ConfigurationDirectory with DynamicUser)."""
    installed_and_running(box)
    owner = box.sh("stat -c '%%u:%%a' %s/status.json" % RUNDIR).strip()
    pid = box.main_pid()
    uid = box.sh("awk '/^Uid:/{print $2}' /proc/%d/status" % pid).strip()
    expect(owner == "%s:644" % uid and uid != "0", "status.json owner/mode %s, service uid %s" % (owner, uid))
    expect("EPERM" not in box.journal() and "Permission denied" not in box.journal(), box.journal())
    expect(box.sh("stat -c '%%U:%%G %%a' %s" % DATA).strip() == "root:root 755", "the data folder changed owner")


@case("service_runs_under_filter")
def service_filter(box):
    """INV-110: the running service carries the seccomp filter and no_new_privs."""
    installed_and_running(box)
    st = box.sh("grep -E '^(Seccomp|NoNewPrivs):' /proc/%d/status" % box.main_pid())
    expect("Seccomp:\t2" in st and "NoNewPrivs:\t1" in st, st)


@case("install_from_installed_program_repairs_its_mode", release=False)
def install_from_installed(box):
    """Install run from /usr/local/bin/aether-serial-bridge itself keeps that copy only when it
    is root:root 0755. A 0700 copy (a restrictive umask, a hand copy) is one the DynamicUser service cannot run:
    install replaces it atomically from itself, and the service starts and listens again."""
    installed_and_running(box)
    box.sh("chmod 0700 %s" % INSTALLED)
    r = box.run("%s --test-op install" % INSTALLED)
    lines = [l for l in r.out.splitlines() if l.strip().startswith("{")]
    expect(lines, "install: %r" % r)
    res = json.loads(lines[-1])
    ok(res, "install from the installed program")
    expect(box.sh("stat -c '%%a %%U:%%G' %s" % INSTALLED).strip() == "755 root:root", "not repaired: %s" % box.sh("stat -c '%%a %%U:%%G' %s" % INSTALLED))
    running_with(box, 4001)


@case("running_time_excludes_suspend", release=False)
def running_time(box):
    """N9-1 on Linux (master "Clock and wait (N9-1)"; WS-5 round 5: the budgets whose expiry ends the process are
    running time): the product's running time is CLOCK_MONOTONIC, which a suspend does not advance, never
    CLOCK_BOOTTIME, which does. A time namespace whose boottime runs one day ahead stands in for a day of
    suspend already behind the machine; RunningTime::now, sampled between the two kernel clocks by the binary
    under test, must follow the monotonic one. (A suspend DURING a wait is HW: see the WS-8 doc.)"""
    day = 86400
    r = box.run("unshare --fork --time --boottime %d %s --test-op clocks" % (day, EXE))
    expect(r.rc == 0, "clocks: %r" % r)
    v = dict(kv.split("=", 1) for kv in r.out.split())
    running, monotonic, boottime = int(v["running"]), int(v["monotonic"]), int(v["boottime"])
    expect(boottime - monotonic >= day * 10**9, "the namespace did not move CLOCK_BOOTTIME: %s" % r.out)
    expect(0 <= running - monotonic < 10**9, "running time does not follow CLOCK_MONOTONIC: %s" % r.out)


@case("failed_apply_survives_reboot")
def failed_apply_survives_reboot(box):
    """INV-48: a failed apply leaves the PERSISTENT mask, which a reboot keeps: the bridge stays down."""
    installed_and_running(box)
    res = box.apply(CONFIG_B, env={"ASB_TEST_FAIL": "firewall:sync"})
    expect(res["outcome"] == "Uncommitted", "the injected failure: %s" % res)
    expect(box.prop("LoadState") == "masked", "not masked after the failure")
    box.reboot()
    expect(box.prop("LoadState") == "masked", "the mask did not survive the reboot (LoadState %s)" % box.prop("LoadState"))
    expect(box.active() in ("inactive", "failed"), "the service is %s after the reboot" % box.active())
    expect(box.prop("NRestarts") in ("0", ""), "restarted")
    expect(box.run("test -L /etc/systemd/system/%s" % UNIT).rc == 0, "the persistent mask link is gone")
    # Recovery: the next apply brings it back.
    ok(box.apply(CONFIG_B), "recovery apply")
    running_with(box, 4002)


@case("interrupted_apply_kill9_then_reboot")
def interrupted_apply(box):
    """INV-48 / INV-73: an apply killed between the firewall sync and the commit, then a reboot: masked, old
    config, no token, nothing runs; the next apply commits."""
    installed_and_running(box)
    before = box.read(DATA + "/config.json")
    box.write("/tmp/b.json", json.dumps(CONFIG_B))
    box.op_paused("apply", ["--config", "/tmp/b.json"], pause="firewall:sync")
    box.kill_ops()
    box.reboot()
    expect(box.prop("LoadState") == "masked", "not masked after the interrupted apply and the reboot")
    expect(box.active() in ("inactive", "failed"), "running after the reboot")
    expect(box.read(DATA + "/config.json") == before, "config.json changed before its commit")
    expect(not box.exists(DATA + "/start.token"), "a start token survived the interrupted apply")
    ok(box.apply(CONFIG_B), "re-apply")
    running_with(box, 4002)


@case("kill_between_sync_and_rename")
def kill_between_sync_and_rename(box):
    """INV-73 (WS-5 deferred): killed after the sync, before the rename: config A stays; without a token no
    start can run anything, even when the unit is unmasked and started by hand."""
    installed_and_running(box)
    before = box.read(DATA + "/config.json")
    box.write("/tmp/b.json", json.dumps(CONFIG_B))
    box.op_paused("apply", ["--config", "/tmp/b.json"], pause="firewall:sync")
    box.kill_ops()
    expect(box.read(DATA + "/config.json") == before, "config.json replaced before the commit")
    box.run("systemctl unmask %s; systemctl start %s" % (UNIT, UNIT))
    box.wait_until(lambda: box.prop("ExecMainStatus") == "6" or box.active() == "active", timeout=20, what="the engine to end")
    expect(box.prop("ExecMainStatus") == "6", "the engine did not refuse with exit 6 (status %s, %s)" % (box.prop("ExecMainStatus"), box.active()))


@case("unit_never_visible_unmasked")
def unit_never_visible_unmasked(box):
    """INV-79: the mask link exists before the unit file (inotify on both directories during install)."""
    box.sh("mkdir -p /usr/local/lib/systemd/system")
    w = box.bg("inotifywait -m -q -e create,moved_to --format '%w%f %e' /etc/systemd/system /usr/local/lib/systemd/system > /tmp/inotify.log 2>&1")
    time.sleep(1.0)
    ok(box.install(), "install")
    time.sleep(0.5)
    box.run("pkill inotifywait")
    w.wait(timeout=10)
    log = [l for l in box.read("/tmp/inotify.log").splitlines() if l.split(" ")[0].endswith("/" + UNIT)]
    mask = next((i for i, l in enumerate(log) if l.startswith("/etc/systemd/system/")), None)
    unit = next((i for i, l in enumerate(log) if l.startswith("/usr/local/lib/systemd/system/")), None)
    expect(mask is not None and unit is not None, "events: %s" % log)
    expect(mask < unit, "the unit file appeared before the mask: %s" % log)


@case("stop_when_not_installed_is_noop")
def stop_not_installed(box):
    """INV-79: stopping a unit that was never installed changes nothing and creates nothing."""
    before = box.sh("ls -A /etc/systemd/system | sort")
    res = box.op("stop")
    expect(res["outcome"] in ("Ok", "NothingChanged"), "stop: %s" % res)
    expect(box.sh("ls -A /etc/systemd/system | sort") == before, "stop changed /etc/systemd/system")
    expect(box.prop("LoadState") == "not-found", "the unit is %s" % box.prop("LoadState"))
    expect(not box.exists("/usr/local/lib/systemd/system/" + UNIT), "a unit file appeared")


@case("kill_after_mask_then_reboot")
def kill_after_mask_then_reboot(box):
    """WS-3 deferred INV-77: the mask link is flushed before anything else; a crash right after it and a
    reboot leave the unit masked."""
    box.op_paused("install", pause="step:after-mask")
    box.kill_ops()
    box.reboot()
    expect(box.prop("LoadState") == "masked", "not masked after the crash and the reboot: %s" % box.prop("LoadState"))


@case("kill_after_mask_before_unit_then_install")
def kill_after_mask_before_unit(box):
    """INV-98: a crash between the mask and the unit file is IncompleteInstall: apply and start refuse, install
    resumes."""
    box.op_paused("install", pause="step:after-mask")
    box.kill_ops()
    expect(box.op("detect")["_rc"] == 0, "detect")
    det = box.run("%s --test-op detect" % SEAM).out.split()
    expect(det and det[0] == "1", "not IncompleteInstall: %s" % det)
    for op in ("start",):
        res = box.op(op)
        expect(res["outcome"] == "NothingChanged" and "--install" in res["message"], "%s: %s" % (op, res))
    res = box.apply(CONFIG_A)
    expect(res["outcome"] == "NothingChanged" and "--install" in res["message"], "apply: %s" % res)
    ok(box.install(), "install resumes")
    expect(box.active() == "active", "not running after the resumed install")


@case("deactivate_keeps_unit_and_binary")
def deactivate_keeps(box):
    """Linux untick: pending written, barrier, the unit stays installed and masked (INV-51's adapter half)."""
    installed_and_running(box)
    box.write("/tmp/b.json", json.dumps(CONFIG_B))
    res = box.op("deactivate", "--config", "/tmp/b.json")
    expect(res["outcome"] == "SavedNotApplied", "deactivate: %s" % res)
    expect(box.prop("LoadState") == "masked", "not masked")
    expect(box.active() in ("inactive", "failed"), "still running")
    expect(box.exists("/usr/local/lib/systemd/system/" + UNIT) and box.exists(INSTALLED), "the unit or binary went away")
    expect(json.loads(box.read(DATA + "/config.pending.json"))["ports"][0]["tcpPort"] == 4002, "pending is not B")
    expect(json.loads(box.read(DATA + "/config.json"))["ports"][0]["tcpPort"] == 4001, "committed is not A")
    expect(not box.exists(DATA + "/start.token"), "untick_deletes_token: the token is still there")


@case("uninstall_removes_everything")
def uninstall(box):
    installed_and_running(box)
    res = box.op("uninstall")
    expect(res["outcome"] == "Ok", "uninstall: %s" % res)
    expect(box.prop("LoadState") == "not-found", "the unit is still known: %s" % box.prop("LoadState"))
    for p in ("/usr/local/lib/systemd/system/" + UNIT, INSTALLED, "/etc/systemd/system/" + UNIT, LOCKDIR + "/engine.lock"):
        expect(not box.exists(p), "%s is still there" % p)
    expect(box.exists(DATA + "/config.json"), "the config was removed")


# ---- WS-16 (INV-125): uninstall's settings, through the CLI ------------------------------------------------------------
STAGING = "/usr/local/lib/aether-serial-bridge"


@case("uninstall_keeps_settings_by_default")
def uninstall_keeps_settings(box):
    installed_and_running(box)
    box.write(DATA + "/config.pending.json", json.dumps(CONFIG_B))
    box.sh("chmod 0644 %s/config.pending.json" % DATA)
    res = box.op("uninstall")
    expect(res["outcome"] == "Ok", "uninstall: %s" % res)
    expect(box.exists(DATA + "/config.json") and box.exists(DATA + "/config.pending.json"), "a settings file was removed")
    expect("The settings are kept in %s." % DATA in res["_out"], "the kept line: %r" % res["_out"])


@case("uninstall_remove_settings")
def uninstall_remove_settings(box):
    installed_and_running(box)
    res = box.op("uninstall", "--remove-settings")
    expect(res["outcome"] == "Ok", "uninstall --remove-settings: %s" % res)
    expect(not box.exists(DATA), "%s is still there" % DATA)
    expect(not box.exists(INSTALLED), "the program is still there")
    expect("The settings were removed." in res["_out"], "the removed line: %r" % res["_out"])


@case("uninstall_untrusted_settings_untouched")
def uninstall_untrusted_settings(box):
    """An untrusted data folder (a group-writable file in it) is touched not at all, and the result says so."""
    installed_and_running(box)
    box.write(DATA + "/planted", "x")
    box.sh("chmod 0666 %s/planted" % DATA)
    before = box.run("ls -la --time-style=+%%s %s" % DATA).out
    res = box.op("uninstall", "--remove-settings")
    expect(res["outcome"] == "Ok", "uninstall: %s" % res)
    expect(box.run("ls -la --time-style=+%%s %s" % DATA).out == before, "the folder changed")
    expect(box.exists(DATA + "/config.json"), "config.json was removed")
    expect("nothing in it was touched" in res["_out"] + res["_err"], "no word of it: %r" % (res["_out"] + res["_err"]))
    expect(not box.exists(INSTALLED), "the rest of the uninstall did not happen")


@case("uninstall_removes_staging_root")
def uninstall_staging_root(box):
    """WS-15's staging root and its staging.* directories go with the uninstall."""
    installed_and_running(box)
    box.sh("mkdir -p %s/staging.test && cp %s %s/staging.test/aether-serial-bridge && chmod -R go-w %s"
           % (STAGING, INSTALLED, STAGING, STAGING))
    res = box.op("uninstall")
    expect(res["outcome"] == "Ok", "uninstall: %s" % res)
    expect(not box.exists(STAGING), "%s is still there" % STAGING)


@case("reboot_autostart")
def reboot_autostart(box):
    """HW 8's container half: an applied bridge comes back after a reboot, and acknowledges its token."""
    installed_and_running(box)
    token = json.loads(box.read(DATA + "/start.token"))["token"]
    box.reboot()
    running_with(box, 4001)
    s = box.status()
    expect(s["startToken"] == token and s["pid"] == box.main_pid(), "status after the reboot: %s" % s)


@case("stop_deletes_token")
def stop_deletes_token(box):
    """INV-113 (WS-5 deferred): Stop revokes the token; the start type is kept; a manual start then exits 6.
    v1.1 station fix: that exit 6 is not a failure (SuccessExitStatus=6): the unit ends inactive, not failed, and
    Restart=on-failure (RestartSec=5) never restarts it; the journal has the one line saying why."""
    installed_and_running(box)
    res = box.op("stop")
    ok(res, "stop")
    expect(not box.exists(DATA + "/start.token"), "the token survived Stop")
    expect(box.prop("UnitFileState") == "enabled", "Stop changed the start type")
    restarts = box.prop("NRestarts")
    box.run("systemctl start %s" % UNIT)
    box.wait_until(lambda: box.prop("ExecMainStatus") == "6", timeout=20, what="exit 6")
    time.sleep(12)   # over two RestartSec: a failure would have been restarted by now
    expect(box.prop("NRestarts") == restarts, "systemd restarted the not-activated service (NRestarts %s -> %s)"
           % (restarts, box.prop("NRestarts")))
    expect(box.prop("ExecMainStatus") == "6", "exit status %s, not 6" % box.prop("ExecMainStatus"))
    expect(box.prop("ActiveState") == "inactive" and box.prop("Result") == "success",
           "not a clean stop: %s / %s" % (box.prop("ActiveState"), box.prop("Result")))
    expect("Not activated (start.token is missing)" in box.journal(), "the journal does not say why")


@case("crash_after_token_before_start_recovery_ok")
def crash_after_token(box):
    """INV-113: killed after R9's token, before start: token, config and generation agree; Start recovers."""
    installed_and_running(box)
    box.write("/tmp/b.json", json.dumps(CONFIG_B))
    box.op_paused("apply", ["--config", "/tmp/b.json"], pause="step:before-allow-starts")
    box.kill_ops()
    expect(box.exists(DATA + "/start.token"), "no token after R9's write")
    expect(json.loads(box.read(DATA + "/config.json"))["ports"][0]["tcpPort"] == 4002, "B not committed")
    ok(box.op("start"), "start")
    running_with(box, 4002)


@case("first_install_token_only_after_commit")
def token_after_commit(box):
    """INV-113: the token is written only after config.json's commit rename (inotify on the data folder)."""
    w = box.bg("mkdir -p %s && inotifywait -m -q -e moved_to,create --format '%%f %%e' %s > /tmp/data.log 2>&1" % (DATA, DATA))
    time.sleep(1.0)
    ok(box.install(), "install")
    time.sleep(0.5)
    box.run("pkill inotifywait")
    w.wait(timeout=10)
    log = box.read("/tmp/data.log").splitlines()
    commit = next((i for i, l in enumerate(log) if l.startswith("config.json MOVED_TO")), None)
    token = next((i for i, l in enumerate(log) if l.startswith("start.token MOVED_TO")), None)
    expect(commit is not None and token is not None and commit < token, "order: %s" % log)


@case("untick_deletes_token")
def untick_deletes_token(box):
    """INV-113 (WS-5 deferred): the Linux untick (Deactivate) revokes start.token; starts stay blocked."""
    installed_and_running(box)
    expect(box.exists(DATA + "/start.token"), "no token before the untick")
    res = box.op("deactivate")
    expect(res["outcome"] in ("Ok", "SavedNotApplied"), "deactivate: %s" % res)
    expect(not box.exists(DATA + "/start.token"), "the token survived the untick")
    expect(box.prop("LoadState") == "masked", "starts not blocked")
