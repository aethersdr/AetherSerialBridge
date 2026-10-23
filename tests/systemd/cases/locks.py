"""The engine lock and the apply lock natively (INV-66/97 twins, INV-92, INV-103, INV-110, INV-116), Service-mode
trust (INV-22, INV-63), the foreign-unit refusal and Repair (INV-74, INV-113)."""

import json
import secrets
import time

from sysd import (SEAM, CONFIG_A, CONFIG_B, DATA, EXE, INSTALLED, LOCKDIR, RUNDIR, UNIT, CaseFail, case, expect,
                  no_stop_claim, ok, running_with)


def installed_and_running(box, config=CONFIG_A):
    ok(box.install(), "install")
    ok(box.apply(config), "apply")
    running_with(box, config["ports"][0]["tcpPort"])


def stopped(box):
    ok(box.op("stop"), "stop")
    box.wait_until(lambda: box.active() in ("inactive", "failed"), what="the service to stop")


def service_exit(box, timeout=30):
    """Starts the unit by hand (as an administrator could), waits for the engine to end; its exit status."""
    box.run("systemctl reset-failed %s; systemctl unmask %s; systemctl start %s" % (UNIT, UNIT, UNIT))
    box.wait_until(lambda: box.active() in ("inactive", "failed", "activating") and box.prop("ExecMainExitTimestampMonotonic") not in ("", "0"),
                   timeout=timeout, what="the engine to exit")
    return box.prop("ExecMainStatus")


# --- INV-22 / INV-63: Service-mode trust ---------------------------------------------------------------------
@case("untrusted_start_writes_nothing")
def untrusted_start(box):
    """INV-22 (Linux untrusted start): an untrusted data folder -> exit 3; nothing written, no status, no
    listener (the service runs under strace in its own unit)."""
    installed_and_running(box)
    stopped(box)
    box.sh("chmod 0777 %s" % DATA)
    box.trace_service()
    box.run("systemctl start %s" % UNIT)
    box.wait_until(lambda: box.prop("ExecMainStatus") == "3", timeout=30, what="exit 3")
    j = box.journal()
    written = [l for l in j.splitlines() if ("O_CREAT" in l or "rename" in l or "unlink" in l) and ("aether-serial-bridge" in l)]
    expect(not written, "the untrusted start wrote: %s" % written[:5])
    expect("status.json" not in j or "O_CREAT" not in j, "status written")
    expect("config.json" not in "\n".join(l for l in j.splitlines() if "openat" in l), "config.json was read")


@case("start_token_symlink_refused")
def token_symlink(box):
    """INV-63 (WS-5 deferred): start.token as a symbolic link makes the folder untrusted: exit 3."""
    installed_and_running(box)
    token = box.read(DATA + "/start.token")
    stopped(box)
    box.write("/root/token", token)
    box.sh("ln -s /root/token %s/start.token" % DATA)
    expect(service_exit(box) == "3", "not refused with exit 3")


@case("config_json_directory_untrusted")
def config_dir(box):
    """INV-63 (WS-5 deferred): config.json as a directory makes the folder untrusted: exit 3; the Applier
    refuses too ("run Repair")."""
    installed_and_running(box)
    stopped(box)
    box.sh("rm %s/config.json && mkdir %s/config.json" % (DATA, DATA))
    expect(service_exit(box) == "3", "not refused with exit 3")
    res = box.apply(CONFIG_B)
    expect(res["outcome"] == "NothingChanged" and "Repair" in res["message"], "apply: %s" % res)


# --- INV-103 ---------------------------------------------------------------------------------------------------
@case("engine_lock_busy_exits_4")
def lock_busy(box):
    installed_and_running(box)
    token = box.read(DATA + "/start.token")
    stopped(box)
    box.write(DATA + "/start.token", token, mode="0644")   # a valid token: only the lock stands in the way
    holder = box.bg("%s --test-op hold-engine-lock 20000" % SEAM)
    time.sleep(1.5)
    expect(service_exit(box) == "4", "not refused with exit 4")
    expect("engine lock" in box.journal(), box.journal()[-800:])
    holder.kill()


@case("lock_held_until_process_exit", release=False)
def lock_held_until_exit(box):
    """INV-103 (native half of inv103-release-in-runner-stop): the engine releases engine.lock only by
    ending; a pause between the orderly stop and Exit::now keeps it held."""
    box.dropin("pause", "[Service]\nEnvironment=ASB_TEST_PAUSE_BEFORE_EXIT_MS=6000\n")
    installed_and_running(box)
    box.run("systemctl stop --no-block %s" % UNIT)
    box.wait_until(lambda: "stopping" in box.journal(), timeout=10, what="the orderly stop")
    time.sleep(1.5)
    r = box.run("%s --test-op try-engine-lock" % SEAM)
    expect(r.out.startswith("Busy"), "engine.lock was free while the engine still lived: %r" % r)
    box.wait_until(lambda: box.active() in ("inactive", "failed"), timeout=30, what="the engine to end")
    r = box.run("%s --test-op try-engine-lock" % SEAM)
    expect(r.out.startswith("Acquired"), "engine.lock not free after the engine ended: %r" % r)


# --- INV-92 -------------------------------------------------------------------------------------------------------
@case("kill9_engine_releases_lock")
def kill9(box):
    installed_and_running(box)
    pid = box.main_pid()
    box.sh("kill -9 %d" % pid)
    t0 = time.time()
    box.wait_until(lambda: box.run("%s --test-op try-engine-lock" % SEAM).out.startswith("Acquired"), timeout=4,
                   what="engine.lock after kill -9")
    expect(time.time() - t0 < 4, "slow")


@case("stopped_before_exit_lock_wait")
def lock_wait(box):
    """INV-92 (native half of inv92-skip-b3): whatever systemd says, the barrier waits for engine.lock. A
    process outside the unit holds it for 6 s (an engine systemd no longer tracks): Stop waits, then succeeds."""
    installed_and_running(box)
    box.run("systemctl stop %s" % UNIT)   # systemd: stopped; the "engine" below still holds the lock
    holder = box.bg("%s --test-op hold-engine-lock 6000" % SEAM)
    box.wait_until(lambda: box.run("%s --test-op try-engine-lock" % SEAM).out.startswith("Busy"), timeout=10, what="the holder")
    t0 = time.time()
    res = box.op("stop")
    took = time.time() - t0
    ok(res, "stop")
    expect(took >= 3.5, "Stop did not wait for engine.lock (%.1fs)" % took)
    holder.wait(timeout=30)


@case("deadline_state_unknown_no_r3")
def deadline(box):
    """INV-92: engine.lock never frees: after the 30 s barrier the apply is StateUnknown and runs nothing from
    R3 on (config.json, pending, token unchanged; starts stay blocked); the cleanup that follows B4 is bounded by
    its own 10 s and the result does not word the stop as proven (WS-5 round 6)."""
    installed_and_running(box)
    box.run("systemctl stop %s" % UNIT)
    holder = box.bg("%s --test-op hold-engine-lock 60000" % SEAM)
    time.sleep(1.5)
    before = box.read(DATA + "/config.json")
    t0 = time.time()
    res = box.apply(CONFIG_B)
    took = time.time() - t0
    expect(res["outcome"] == "StateUnknown", "apply: %s" % res)
    # B4 runs the cleanup (WS-5 round 6): its own barrier waits for engine.lock again within 10 s. Both waits
    # end at their deadlines (acquireUntil honours them), and nothing claims the stop.
    expect(38 <= took < 55, "the 30 s barrier plus the 10 s cleanup took %.1fs" % took)
    no_stop_claim(res, "apply")
    expect(box.read(DATA + "/config.json") == before, "config.json changed")
    expect(not box.exists(DATA + "/config.pending.json"), "pending written after the deadline")
    expect(not box.exists(DATA + "/start.token"), "a token exists")
    expect(box.prop("LoadState") == "masked", "starts not blocked")
    holder.kill()


@case("service_started_while_applier_holds_lock_fails_closed")
def started_while_held(box):
    """INV-92: the manager starts the engine while the Applier holds engine.lock (an administrator unmasks and
    starts it mid-apply): the engine exits 4, nothing listens."""
    installed_and_running(box)
    box.write("/tmp/b.json", json.dumps(CONFIG_B))
    box.op_paused("apply", ["--config", "/tmp/b.json"], pause="firewall:sync")
    status = service_exit(box)
    expect(status == "4", "the engine did not refuse with exit 4 (%s)" % status)
    expect(box.run("ss -ltn 2>/dev/null | grep -E ':(4001|4002) '").rc != 0 if box.run("command -v ss").rc == 0 else True, "something listens")
    box.kill_ops()


@case("paused_launch_after_interrupted_apply")
def paused_launch(box):
    """INV-113: an apply dies after the barrier; a launch that reaches engine.lock afterwards finds no token
    and exits 6 without reading config."""
    installed_and_running(box)
    box.write("/tmp/b.json", json.dumps(CONFIG_B))
    box.op_paused("apply", ["--config", "/tmp/b.json"], pause="firewall:sync")
    box.kill_ops()
    box.trace_service("openat")
    status = service_exit(box)
    expect(status == "6", "exit %s, expected 6" % status)
    reads = [l for l in box.journal().splitlines() if "config.json" in l and "openat" in l]
    expect(not reads, "config.json read without a token: %s" % reads)


@case("incomplete_install_lock_bootstrap")
def incomplete_bootstrap(box):
    """INV-92 / INV-116: IncompleteInstall (the program went missing) is resumed by install without recreating a
    matching lock pair: same id, same inode."""
    installed_and_running(box)
    ids = box.lock_ids()
    inode = box.sh("stat -c %%i %s/engine.lock" % LOCKDIR).strip()
    box.sh("rm %s" % INSTALLED)
    res = box.apply(CONFIG_B)
    expect(res["outcome"] == "NothingChanged" and "--install" in res["message"], "apply on IncompleteInstall: %s" % res)
    ok(box.install(), "install")
    expect(box.lock_ids() == ids, "the lock pair was recreated over a matching pair")
    expect(box.sh("stat -c %%i %s/engine.lock" % LOCKDIR).strip() == inode, "engine.lock was replaced")
    expect(box.active() == "active", "not running")


# --- INV-116 ----------------------------------------------------------------------------------------------------
@case("overlay_lock_dir_refused", lockdir=False)
def overlay(box):
    """INV-116: the lock directory on overlayfs (the container's root) is refused before any mutation."""
    res = box.install()
    expect(res["outcome"] == "NothingChanged" and "overlayfs" in res["message"], "install: %s" % res)
    expect(not box.exists(LOCKDIR), "the lock directory was created")
    expect(not box.exists("/etc/systemd/system/" + UNIT), "the unit was masked or created")


@case("tmpfs_lock_dir_refused", lockdir=False)
def tmpfs(box):
    """INV-116: tmpfs at the lock directory (lost at every boot) is refused; NFS/CIFS/FUSE are covered by
    tst_EngineLockPosix (injected f_type) and the qualification host."""
    box.sh("mkdir -p %s && mount -t tmpfs tmpfs %s && chmod 0755 %s" % (LOCKDIR, LOCKDIR, LOCKDIR))
    res = box.install()
    expect(res["outcome"] == "NothingChanged" and "tmpfs" in res["message"], "install: %s" % res)
    expect(box.sh("ls -A %s" % LOCKDIR).strip() == "", "something was written in the lock directory")


@case("lock_identity_mismatch_refused_before_mutation")
def identity_mismatch(box):
    """INV-116: a record that does not match the lock: the prologue refuses (nothing changed) and the engine
    exits 4 before it reads config.json."""
    installed_and_running(box)
    stopped(box)
    box.write(LOCKDIR + "/engine.lock.id", '{"lockId":"%s"}' % ("0" * 32), mode="0644")
    before = box.sh("ls -l --time-style=+%%s.%%N %s; systemctl show -p LoadState -p UnitFileState %s" % (DATA, UNIT))
    res = box.op("start")
    expect(res["outcome"] == "NothingChanged" and "lock record mismatch" in res["message"], "start: %s" % res)
    expect(box.sh("ls -l --time-style=+%%s.%%N %s; systemctl show -p LoadState -p UnitFileState %s" % (DATA, UNIT)) == before,
           "something changed")
    box.trace_service("openat")
    expect(service_exit(box) == "4", "the engine did not exit 4")
    reads = [l for l in box.journal().splitlines() if "config.json" in l and "openat" in l]
    expect(not reads, "config.json read before the identity check: %s" % reads)


@case("identity_survives_reboot")
def identity_reboot(box):
    """INV-116 (D58): identity is the id inside the files, not st_dev/st_ino. The lock files are re-created with
    the same bytes (new inodes, as after a restore or a changed device enumeration) and the box reboots: the
    pair still matches; Start works."""
    installed_and_running(box)
    stopped(box)
    for f in ("engine.lock", "engine.lock.id"):
        box.sh("cd %s && cp -p %s x.tmp && mv x.tmp %s" % (LOCKDIR, f, f))
    box.reboot()
    ok(box.op("start"), "start after the reboot")
    running_with(box, 4001)


@case("insecure_lock_file_replaced")
def insecure_file(box):
    """INV-116 (WS-5 round 2): engine.lock not root:root 0644 -> InsecureFile: the engine exits 4, Start refuses,
    install replaces it with a new id after the engine-free proof."""
    installed_and_running(box)
    old = box.lock_ids()
    stopped(box)
    box.sh("chmod 0666 %s/engine.lock" % LOCKDIR)
    res = box.op("start")
    expect(res["outcome"] in ("NothingChanged", "StateUnknown") and ("--install" in res["message"] or "Repair" in res["message"]),
           "start: %s" % res)
    ok(box.install(), "install")
    new = box.lock_ids()
    expect(new[0] == new[1] and new != old, "the pair was not replaced: %s -> %s" % (old, new))
    expect(box.sh("stat -c %%a %s/engine.lock" % LOCKDIR).strip() == "644", "mode not restored")
    expect(box.active() == "active", "not running")


@case("insecure_lock_file_replaced_by_repair")
def insecure_file_repair(box):
    installed_and_running(box)
    old = box.lock_ids()
    stopped(box)
    box.sh("chown 1000 %s/engine.lock" % LOCKDIR)
    res = box.op("repair")
    expect(res["outcome"] in ("Ok", "CommittedNotRunning", "SavedNotApplied") or "engine.lock" in " ".join(res.get("fileChanges", [])),
           "repair: %s" % res)
    new = box.lock_ids()
    expect(new != old and box.sh("stat -c %%u %s/engine.lock" % LOCKDIR).strip() == "0", "not replaced: %s" % res)
    ok(box.op("start"), "start after Repair")


def unreadable_record_row(recover):
    """INV-116: an engine.lock.id root can read but the DynamicUser service cannot (0600) is
    not a matching pair. Before the fix Repair saw "matching", did nothing, and every start then failed the
    service's identity check. Now normal operations refuse it, `recover` (install or Repair) replaces the pair,
    and the service starts and listens."""
    def fn(box):
        installed_and_running(box)
        old = box.lock_ids()
        stopped(box)
        box.sh("chmod 0600 %s/engine.lock.id" % LOCKDIR)
        res = box.op("start")
        expect(res["outcome"] != "Ok", "start over an unreadable record: %s" % res)
        res = box.op(recover)
        expect(res["outcome"] in ("Ok", "CommittedNotRunning", "SavedNotApplied"), "%s: %s" % (recover, res))
        new = box.lock_ids()
        expect(new[0] == new[1] and new != old, "the pair was not replaced: %r" % (new,))
        expect(box.sh("stat -c %%a %s/engine.lock.id" % LOCKDIR).strip() == "644", "the record is not 0644")
        if box.active() != "active":
            ok(box.op("start"), "start after %s" % recover)
        running_with(box, 4001)
    return fn


case("unreadable_lock_record_replaced(repair)")(unreadable_record_row("repair"))
case("unreadable_lock_record_replaced(install)")(unreadable_record_row("install"))


@case("insecure_lock_directory_refused")
def insecure_dir(box):
    """INV-116: a lock directory not root:root 0755 is never replaced: every operation refuses and names the
    manual step; nothing is written in it."""
    installed_and_running(box)
    stopped(box)
    box.sh("chmod 0777 %s" % LOCKDIR)
    listing = box.sh("ls -la --time-style=+%%s.%%N %s" % LOCKDIR)
    for op in ("install", "repair", "start"):
        res = box.op(op)
        expect(res["outcome"] != "Ok", "%s succeeded: %s" % (op, res))
        expect("administrator" in res["message"] or "directory" in res["message"], "%s: %s" % (op, res))
    expect(box.sh("ls -la --time-style=+%%s.%%N %s" % LOCKDIR) == listing, "the lock directory changed")
    expect(service_exit(box) == "4", "the engine did not exit 4")


def old_engine_row(kind):
    """INV-116 / WS-5 rounds 3-4 (the native half of the repair-folder-before-proof and acquired-mismatch
    cases): an engine that outlives its stop still holds its engine.lock inode while
    install, Repair and --force-recreate run. `kind` is what they find at the lock's name:
      unlinked           nothing (the held inode was unlinked): B3 ends Missing
      insecure           the held file itself, now mode 0666: B3 ends InsecureFile
      replaced-other-id  a replacement root:root 0644 engine.lock with another id, free to acquire: B3 holds it,
                         but its id is not the recorded one, so the hold proves nothing
    In each, the engine-free proof decides (the adapter's query reports the surviving pid): none of the three
    moves the data folder, touches the pair or reports Ok while the old engine lives, and no result words the
    stop as proven. Once it is gone they succeed. The engine is held after its orderly stop by the test seam
    ASB_TEST_PAUSE_BEFORE_EXIT_MS (its kill deadline ends it 20 s after the stop request). SIGSTOP would not
    do: systemd follows SIGTERM with SIGCONT."""
    def fn(box):
        box.dropin("linger", "[Service]\nEnvironment=ASB_TEST_PAUSE_BEFORE_EXIT_MS=60000\nTimeoutStopSec=90\n")
        installed_and_running(box)
        pid = box.main_pid()
        lock, record = box.lock_ids()
        if kind == "unlinked":
            box.sh("rm %s/engine.lock" % LOCKDIR)
        elif kind == "insecure":
            box.sh("chmod 0666 %s/engine.lock" % LOCKDIR)
        else:
            other = secrets.token_hex(16)
            expect(other != record.strip(), "the replacement id collides")
            box.sh("rm %s/engine.lock" % LOCKDIR)
            box.write(LOCKDIR + "/engine.lock", other + lock[len(lock.rstrip("\n")):], mode="0644")
            box.sh("chown root:root %s/engine.lock" % LOCKDIR)
            expect(box.run("%s --test-op try-engine-lock" % SEAM).out.startswith("Acquired"), "the replacement is not free")
        pair = box.sh("ls -li %s; cat %s/engine.lock* 2>/dev/null; true" % (LOCKDIR, LOCKDIR))
        # An untrusted data folder: Repair and --force-recreate would move it aside, which must wait for the proof.
        box.sh("chmod 0777 %s" % DATA)
        folder_inode = box.sh("stat -c %%i %s" % DATA).strip()
        for args in (("install",), ("repair",), ("install", "--force-recreate")):
            res = box.op(*args)
            expect(box.run("test -d /proc/%d" % pid).rc == 0, "the old engine ended too early (%s)" % (args,))
            expect(res["outcome"] != "Ok", "%s succeeded while the old engine lives: %s" % (args, res))
            no_stop_claim(res, " ".join(args))
            expect(box.sh("ls -li %s; cat %s/engine.lock* 2>/dev/null; true" % (LOCKDIR, LOCKDIR)) == pair,
                   "%s changed the lock pair while the old engine lives" % (args,))
            expect(box.sh("stat -c %%i %s" % DATA).strip() == folder_inode, "%s moved the data folder" % (args,))
        box.wait_until(lambda: box.run("test -d /proc/%d" % pid).rc != 0, timeout=40, what="the old engine to end")
        expect(not box.run("ls -d /etc/aether-serial-bridge.untrusted-* 2>/dev/null").out.strip(), "a folder was moved aside")
        ok(box.op("install", "--force-recreate"), "install --force-recreate after the old engine ended")
        expect(box.run("ls -d /etc/aether-serial-bridge.untrusted-*").rc == 0, "the untrusted folder was not moved aside")
        lock2, record2 = box.lock_ids()
        expect(lock2 == record2, "the pair does not match after the recovery: %r %r" % (lock2, record2))
        expect(box.sh("stat -c %%a %s/engine.lock" % LOCKDIR).strip() == "644", "engine.lock not 0644")
        ok(box.apply(CONFIG_A), "apply")
        running_with(box, 4001)
    return fn


case("old_engine_on_unlinked_lock_blocks_repair", release=False)(old_engine_row("unlinked"))
case("old_inode_held_blocks_recreation", release=False)(old_engine_row("unlinked"))   # the name WS-5's acceptance uses
for _kind in ("insecure", "replaced-other-id"):
    case("old_engine_on_unlinked_lock_blocks_repair(%s)" % _kind, release=False)(old_engine_row(_kind))


# --- INV-74 / INV-113: Repair --------------------------------------------------------------------------------------
@case("repair_untrusted_folder_touches_nothing_inside", release=False)
def repair_untrusted(box):
    """INV-74 POSIX twin / INV-113: an untrusted folder is renamed aside whole: its token is never opened,
    revoked or deleted, a subdirectory and a link survive."""
    installed_and_running(box)
    token = box.read(DATA + "/start.token")
    box.sh("mkdir %s/sub && echo inner > %s/sub/f && ln -s /etc/hostname %s/link && chmod 0777 %s" % (DATA, DATA, DATA, DATA))
    r = box.run("strace -f -qq -e trace=openat,unlink,unlinkat,truncate,ftruncate -o /tmp/repair.trace %s --test-op repair" % SEAM)
    expect(r.rc in (0, 1), "%r" % r)
    aside = box.sh("ls -d /etc/aether-serial-bridge.untrusted-*").split()
    expect(len(aside) == 1, "aside: %s" % aside)
    a = aside[0]
    expect(box.read(a + "/start.token") == token, "the token inside the untrusted folder changed")
    expect(box.read(a + "/sub/f") == "inner\n" and box.run("test -L %s/link" % a).rc == 0, "contents changed")
    t = box.read("/tmp/repair.trace")
    touched = [l for l in t.splitlines() if "start.token" in l or "/sub" in l]
    expect(not touched, "Repair touched the untrusted folder's contents: %s" % touched[:5])
    expect(box.exists(DATA) and box.sh("stat -c %%a %s" % DATA).strip() == "755", "no fresh folder")


# --- a unit this program did not write -------------------------------------------------------------------------
FOREIGN_UNIT = """[Unit]
Description=Someone else's aether-serial-bridge unit

[Service]
Type=simple
ExecStart=/usr/local/lib/other-bridge/aether-serial-bridge --run
DynamicUser=yes

[Install]
WantedBy=multi-user.target
"""


@case("foreign_unit_refused")
def foreign_unit(box):
    """A unit this program did not write, in /etc/systemd/system under its unit name: install, apply and start
    refuse (Foreign) and nothing changes; after removing it, install succeeds."""
    box.write("/etc/systemd/system/" + UNIT, FOREIGN_UNIT)
    box.sh("systemctl daemon-reload")
    before = box.sh("ls -la /etc/systemd/system /usr/local/bin; ls -la %s 2>&1; ls -A %s" % (DATA, LOCKDIR))
    for res in (box.install(), box.apply(CONFIG_A), box.op("start")):
        expect(res["outcome"] == "NothingChanged", "not refused: %s" % res)
        expect("not one this program installed" in res["message"], res["message"])
    expect(box.sh("ls -la /etc/systemd/system /usr/local/bin; ls -la %s 2>&1; ls -A %s" % (DATA, LOCKDIR)) == before, "something changed")
    box.sh("rm /etc/systemd/system/%s && systemctl daemon-reload" % UNIT)
    ok(box.install(), "install after the foreign unit is gone")


# --- apply.lock (INV-66 / INV-97 twins) ------------------------------------------------------------------------
@case("lock_three_process_handoff", release=False)
def three(box):
    procs = [box.bg("%s --test-op hold-apply-lock 1500" % SEAM) for _ in range(3)]
    spans = []
    for p in procs:
        out, _ = p.communicate(timeout=60)
        lines = out.decode().split()
        expect("HELD" in lines and "RELEASED" in lines, out)
        spans.append((int(lines[lines.index("HELD") + 1]), int(lines[lines.index("RELEASED") + 1])))
    spans.sort()
    for a, b in zip(spans, spans[1:]):
        expect(a[1] <= b[0], "holders overlapped: %s" % spans)


@case("lock_holder_killed", release=False)
def holder_killed(box):
    p = box.bg("%s --test-op hold-apply-lock 600000" % SEAM)
    box.wait_until(lambda: box.run("grep -c aether-serial-bridge.apply.lock /proc/locks >/dev/null; "
                                   "test -e /run/aether-serial-bridge.apply.lock").rc == 0, timeout=10, what="the holder")
    time.sleep(1)
    box.run("pkill -9 -f 'hold-apply-lock'")
    r = box.run("timeout 10 %s --test-op hold-apply-lock 1" % SEAM)
    expect("HELD" in r.out, "not acquired after the holder was killed: %r" % r)
    p.kill()


@case("standard_user_cannot_open_or_create_apply_lock", release=False)
def standard_user(box):
    box.run("%s --test-op hold-apply-lock 1" % SEAM)
    expect(box.sh("stat -c '%U %a' /run/aether-serial-bridge.apply.lock").strip() == "root 600", "mode")
    r = box.run("runuser -u tuser -- sh -c 'exec 3</run/aether-serial-bridge.apply.lock'")
    expect(r.rc != 0 and "Permission denied" in r.err, "a standard user opened apply.lock: %r" % r)
    r = box.run("runuser -u tuser -- touch /run/x.lock")
    expect(r.rc != 0, "a standard user can create files in /run")


@case("wide_apply_lock_mode_reset", release=False)
def wide_mode(box):
    box.run("%s --test-op hold-apply-lock 1" % SEAM)
    box.sh("chmod 0666 /run/aether-serial-bridge.apply.lock")
    r = box.run("%s --test-op hold-apply-lock 1" % SEAM)
    expect("HELD" in r.out, "%r" % r)
    expect(box.sh("stat -c %a /run/aether-serial-bridge.apply.lock").strip() == "600", "mode not reset")
