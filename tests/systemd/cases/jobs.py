"""D-Bus job tracking against a real PID 1 (INV-115) and the lock-pair crash rows (INV-116, WS-5 deferred)."""

import json
import re
import time

from sysd import SEAM, CONFIG_A, CONFIG_B, DATA, EXE, LOCKDIR, UNIT, case, expect, no_stop_claim, ok, running_with


def installed_and_running(box, config=CONFIG_A):
    ok(box.install(), "install")
    ok(box.apply(config), "apply")
    running_with(box, config["ports"][0]["tcpPort"])


@case("jobremoved_failure_reported")
def jobremoved_failure(box):
    """INV-115: a start job systemd ends as "failed" is a failed step (CommittedNotRunning), not a success."""
    installed_and_running(box)
    box.dropin("fail", "[Service]\nExecStartPre=/bin/false\n")
    res = box.apply(CONFIG_B)
    expect(res["outcome"] == "CommittedNotRunning", "apply: %s" % res)
    expect("failed" in res["message"], res["message"])


def bus_events(text):
    """(kind, detail) in order from `busctl monitor` text: ("call", member) and ("removed", unit, result)."""
    events = []
    for block in re.split(r"\n(?=‣ )", text):
        m = re.search(r"Member=(\w+)", block)
        if not m:
            continue
        member = m.group(1)
        if "Type=method_call" in block:
            events.append(("call", member))
        elif "Type=signal" in block and member == "JobRemoved":
            strings = re.findall(r'STRING "([^"]*)"', block)
            path = re.findall(r'OBJECT_PATH "([^"]*)"', block)
            events.append(("removed", strings[0] if strings else "", strings[1] if len(strings) > 1 else "", path[0] if path else ""))
    return events


@case("outstanding_stop_job_reconciled_before_start")
def outstanding_stop_job(box):
    """INV-115: a stop job still running at R9 (ExecStopPost keeps it busy after the engine ended, so the
    barrier already holds engine.lock) is settled before StartUnit is sent."""
    installed_and_running(box)
    box.dropin("slowstop", "[Service]\nExecStopPost=/bin/sleep 6\n")
    mon = box.bg("busctl monitor org.freedesktop.systemd1 > /tmp/bus.log 2>&1")
    time.sleep(1.0)
    res = box.apply(CONFIG_B)
    time.sleep(0.5)
    box.run("pkill busctl")
    mon.wait(timeout=10)
    ok(res, "apply")
    ev = bus_events(box.read("/tmp/bus.log") or "")
    stop = max(i for i, e in enumerate(ev) if e == ("call", "StopUnit"))
    start = next(i for i, e in enumerate(ev) if e == ("call", "StartUnit") and i > stop)
    removed = [i for i, e in enumerate(ev) if e[0] == "removed" and e[1] == UNIT and stop < i]
    expect(removed and removed[0] < start, "StartUnit was sent while the stop job was outstanding: %s" % ev[stop:start + 3])
    expect(("call", "ListJobs") in ev[stop:start], "no ListJobs before StartUnit: %s" % ev[stop:start + 1])
    running_with(box, 4002)


@case("stop_deadline_cleanup_keeps_start_type")
def stop_cleanup(box):
    """WS-5 round 6 (B4 -> failClosed; Stop's stop-only cleanup): engine.lock stays held past Stop's 30 s
    barrier (a process outside the unit holds it). Stop ends StateUnknown; its cleanup requests the stop a second
    time and waits for engine.lock once more within its own 10 s, and never blocks starts: StopUnit twice, no
    MaskUnitFiles or DisableUnitFiles, LoadState and UnitFileState as they were, and no result words the stop as
    proven. Both waits end at their deadlines (acquireUntil honours them)."""
    installed_and_running(box)
    box.run("systemctl stop %s" % UNIT)
    holder = box.bg("%s --test-op hold-engine-lock 75000" % SEAM)
    box.wait_until(lambda: box.run("%s --test-op try-engine-lock" % SEAM).out.startswith("Busy"), timeout=10, what="the holder")
    before = (box.prop("LoadState"), box.prop("UnitFileState"))
    expect(before[0] == "loaded", "starts already blocked before Stop: %s" % (before,))
    mon = box.bg("busctl monitor org.freedesktop.systemd1 > /tmp/bus.log 2>&1")
    time.sleep(1.0)
    t0 = time.time()
    res = box.op("stop")
    took = time.time() - t0
    time.sleep(0.5)
    box.run("pkill busctl")
    mon.wait(timeout=10)
    expect(res["outcome"] == "StateUnknown", "stop: %s" % res)
    no_stop_claim(res, "stop")
    expect(38 <= took < 55, "the 30 s barrier plus the 10 s cleanup took %.1fs" % took)
    calls = [e[1] for e in bus_events(box.read("/tmp/bus.log") or "") if e[0] == "call"]
    expect(calls.count("StopUnit") >= 2, "the cleanup did not request the stop again: %s" % calls)
    blocked = [c for c in calls if c in ("MaskUnitFiles", "DisableUnitFiles")]
    expect(not blocked, "Stop blocked starts: %s" % blocked)
    expect((box.prop("LoadState"), box.prop("UnitFileState")) == before, "the start type changed: %s -> %s" %
           (before, (box.prop("LoadState"), box.prop("UnitFileState"))))
    holder.kill()


# --- INV-116: crash between the lock-pair publication steps, then install / Repair converge -------------------
POINTS = {
    "temp-lock": "sys:before:renameat:engine.lock",
    "lock-renamed": "sys:after:renameat:engine.lock",
    "temp-record": "sys:before:renameat:engine.lock.id",
    "record-renamed": "sys:after:renameat:engine.lock.id",
}


def crash_row(point, recover):
    def fn(box):
        box.op_paused("install", pause=POINTS[point])
        box.kill_ops()
        res = box.op("start")
        expect(res["outcome"] != "Ok", "start succeeded over a half-published pair: %s" % res)
        ok(box.op(recover), recover)
        lock, record = box.lock_ids()
        expect(lock and lock == record, "the pair did not converge: %r / %r" % (lock, record))
        leftovers = box.sh("ls -A %s" % LOCKDIR).split()
        expect(sorted(leftovers) == ["engine.lock", "engine.lock.id"], "leftovers: %s" % leftovers)
        if recover == "repair":
            ok(box.apply(CONFIG_A), "apply after Repair")
        running_with(box, 4001) if recover == "repair" else box.wait_until(lambda: box.active() == "active", what="running")
    return fn


for _p in POINTS:
    case("lock_pair_crash_between_steps_then_reinstall(%s)" % _p)(crash_row(_p, "install"))
case("lock_pair_crash_between_steps_then_repair(lock-renamed)")(crash_row("lock-renamed", "repair"))


@case("prologue_refuses_partial_pair_for_normal_operations")
def partial_pair(box):
    """INV-116: a partial pair (engine.lock without its record) is refused by normal operations before any
    change; install recovers it (record published from the id read through the held lock)."""
    installed_and_running(box)
    ok(box.op("stop"), "stop")
    lock = box.read(LOCKDIR + "/engine.lock")
    box.sh("rm %s/engine.lock.id" % LOCKDIR)
    for op in ("start", "stop", "deactivate"):
        res = box.op(op)
        # Refused before any change: the missing record also makes detection say IncompleteInstall.
        expect(res["outcome"] == "NothingChanged" and ("lock record mismatch" in res["message"] or "--install" in res["message"]),
               "%s: %s" % (op, res))
    ok(box.install(), "install")
    expect(box.lock_ids() == (lock, lock), "the record does not carry the held lock's id")
