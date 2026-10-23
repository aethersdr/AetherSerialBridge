"""The Linux window's staged route (v1.1 WS-15, D60; INV-122, INV-123, INV-124) through the REAL pkexec and polkit, as
uid 1000: `pkexec /bin/sh -c <stage.sh> asb-stage <sha256> <plen> <rlen>` with the window's program, then the request,
on stdin. asb_stage_client (tests/systemd/stage_client.cpp) is the window without the window: the window's routing and
HelperClient over a copy of PkexecClient's launch, or the raw launch with the framing under the case's control.

Every case asserts the script's stderr against the catalogue (tests/systemd/stage_catalogue.json): the exact
"asb-stage:" line on an exit of the script's own, and no "asb-stage:" line at all on the staged helper's 0/1/2/5 and on
pkexec's 126/127, so a right exit code from the wrong step fails.

Polkit set-ups: a YES (or NO) rule for org.freedesktop.policykit.exec restricted to program "/bin/sh" and user tuser;
no rule and no agent (records what pkexec says); and a real auth_admin prompt answered by pkttyagent on a
pseudo-terminal with the test administrator's password (tadmin, in group sudo; the image's test user)."""

import base64
import hashlib
import json
import os
import pathlib
import shlex
import time

import sysd
from sysd import CONFIG_A, CONFIG_B, DATA, EXE, INSTALLED, SEAM_DIR, UNIT, CaseFail, CaseSkip, case, expect, running_with

CLIENT = SEAM_DIR + "/asb_stage_client"
ROOT = "/usr/local/lib/aether-serial-bridge"
POLICY = "/usr/share/polkit-1/actions/org.aethersdr.aetherserialbridge.policy"
UNIT_FILE = "/usr/local/lib/systemd/system/" + UNIT
EXEC_ACTION = "org.freedesktop.policykit.exec"
HELPER_ACTION = "org.aethersdr.aetherserialbridge.helper"
# The policy and action every release up to v1.1.0 installed (the v1.0 fixture's): the identifiers moved to
# org.aethersdr on 2026-10-22, and this build's install, update and uninstall remove the old file.
LEGACY_POLICY = "/usr/share/polkit-1/actions/agency.cloaked.aetherserialbridge.policy"
LEGACY_HELPER_ACTION = "agency.cloaked.aetherserialbridge.helper"
RULES = "/etc/polkit-1/rules.d/50-asb-test.rules"
ADMIN, ADMIN_PASSWORD = "tadmin", "asb-test-admin"   # the image's test administrator (systemd-bookworm.Dockerfile)
HERE = pathlib.Path(__file__).resolve().parent.parent
CATALOGUE = json.loads((HERE / "stage_catalogue.json").read_text())["lines"]
REFUSED_LIFECYCLE = "error: install and uninstall run only from the window's staged program; nothing was changed."


# --- the catalogue ------------------------------------------------------------------------------------------------
def catalogue_line(exit_code, line):
    """The catalogue's entry for `line` (placeholders filled in by the caller); fails when the line is not one."""
    for e in CATALOGUE:
        pat = e["line"]
        for ph in ("<dir>", "<plen>", "<rlen>", "<rc>"):
            pat = pat.replace(ph, "\x00")
        parts = pat.split("\x00")
        if len(parts) == 1 and pat == line or len(parts) > 1 and line.startswith(parts[0]) and line.endswith(parts[-1]):
            expect(e["exit"] == exit_code, "the catalogue gives %r exit %d, not %d" % (line, e["exit"], exit_code))
            return e
    raise CaseFail("%r is not in the stderr catalogue" % line)


def stage_lines(err):
    return [l[len("asb-stage: "):] for l in (err or "").splitlines() if l.startswith("asb-stage: ")]


def expect_stage_line(res, exit_code, line, what):
    """The script's own exit: exactly `exit_code` and exactly one "asb-stage: <line>" (from the catalogue)."""
    catalogue_line(exit_code, line)
    got = stage_lines(res["err"])
    expect(res["rc"] == exit_code and got == [line],
           "%s: expected exit %d with %r, got exit %s with %r (stderr %r)" % (what, exit_code, line, res["rc"], got, res["err"][-1500:]))


def expect_no_stage_line(res, what):
    expect(not stage_lines(res["err"]), "%s: an asb-stage line on exit %s: %r" % (what, res["rc"], res["err"][-1500:]))


# --- polkit set-ups ---------------------------------------------------------------------------------------------------
def exec_rule(box, answer, helper_answer=None):
    """A rule answering `answer` for the generic exec action, only for program /bin/sh and user tuser (and, when given,
    `helper_answer` for the bridge's own helper action)."""
    text = ("polkit.addRule(function(action, subject) {\n"
            "    if (action.id == \"%s\" && action.lookup(\"program\") == \"/bin/sh\" && subject.user == \"tuser\")\n"
            "        return polkit.Result.%s;\n" % (EXEC_ACTION, answer))
    if helper_answer:
        text += ("    if (action.id == \"%s\" && subject.user == \"tuser\") return polkit.Result.%s;\n" % (HELPER_ACTION, helper_answer))
    text += "});\n"
    box.write(RULES, text)
    box.sh("chmod 644 %s && systemctl restart polkit" % RULES)


def helper_rule(box, answer, action=HELPER_ACTION):
    box.write(RULES, "polkit.addRule(function(action, subject) {\n    if (action.id == \"%s\" && subject.user == \"tuser\") "
                     "return polkit.Result.%s;\n});\n" % (action, answer))
    box.sh("chmod 644 %s && systemctl restart polkit" % RULES)


def no_rule(box):
    box.run("rm -f %s" % RULES)
    box.sh("systemctl restart polkit")


# --- the client -------------------------------------------------------------------------------------------------------
def client(box, args, user="tuser", timeout=300):
    r = box.run("%s %s" % (CLIENT, " ".join(shlex.quote(a) for a in args)), user=user, timeout=timeout)
    lines = [l for l in r.out.splitlines() if l.startswith("{\"")]
    expect(r.rc == 0 and lines, "asb_stage_client %s: rc=%s out=%r err=%r" % (args[:2], r.rc, r.out[-1500:], r.err[-1500:]))
    res = json.loads(lines[-1])
    res["_client_err"] = r.err
    return res


def put(box, path, data, mode="644"):
    if isinstance(data, str):
        data = data.encode()
    box.write(path, base64.b64encode(data).decode() + "\n")
    box.sh("base64 -d < {0} > {0}.d && mv {0}.d {0} && chmod {1} {0}".format(shlex.quote(path), mode))


def request_file(box, req, path="/tmp/stage-request.json"):
    put(box, path, json.dumps(req, separators=(",", ":")) + "\n")
    return path


def config_file(box, config, path="/tmp/stage-config.json"):
    put(box, path, json.dumps(config))
    return path


def window(box, action, *extra, user="tuser", timeout=300):
    """What the window does for `action`, through the real pkexec (its program: the program under test, EXE)."""
    return client(box, ["window", action, "--program", EXE] + list(extra), user=user, timeout=timeout)


def raw(box, program, req, *extra, user="tuser", timeout=300):
    return client(box, ["raw", "--program", program, "--request", request_file(box, req)] + list(extra), user=user, timeout=timeout)


def sha_of(box, path):
    return box.sh("sha256sum %s | cut -c1-64" % shlex.quote(path)).strip()


def expect_root_clean(box, what, must_exist=True):
    """The staging root holds no staging.* directory (and, when it exists, is root:root 0700)."""
    if not box.exists(ROOT):
        expect(not must_exist, "%s: the staging root does not exist" % what)
        return
    expect(box.sh("stat -c %%u:%%g:%%a %s" % ROOT).strip() == "0:0:700", "%s: the staging root is not root:root 0700" % what)
    left = box.sh("ls -A %s" % ROOT).split()
    expect(not left, "%s: the staging root still holds %r" % (what, left))


def nothing_installed(box, what):
    for p in (INSTALLED, UNIT_FILE, POLICY, LEGACY_POLICY):
        expect(not box.exists(p), "%s: %s exists" % (what, p))


def audit(box):
    return box.run("journalctl --no-pager -o cat -t aether-serial-bridge").out


# --- cases ------------------------------------------------------------------------------------------------------------
@case("bootstrap_install_from_window")
def install_from_window(box):
    """Nothing installed -> the window's staged install with its ports: the service runs that config, the installed
    program is the payload byte for byte, the policy file is present, the staging root is empty (only its 0700
    directory), the journal's audit line says staged and uid 1000; no asb-stage line."""
    exec_rule(box, "YES")
    nothing_installed(box, "before")
    res = window(box, "install", "--config", config_file(box, CONFIG_A))
    expect_no_stage_line(res, "install")
    expect(res["rc"] == 0 and res["kind"] == "Done" and res["route"] == "staged" and res["op"] == "install",
           "install: %r" % res)
    expect(res["clearsDirty"], "the staged install with ports did not clear dirty: %r" % res)
    running_with(box, CONFIG_A["ports"][0]["tcpPort"])
    expect(sha_of(box, INSTALLED) == sha_of(box, EXE), "the installed program is not the payload")
    expect(box.exists(POLICY), "no polkit policy after the staged install")
    expect_root_clean(box, "after the install")
    a = audit(box)
    expect("aether-serial-bridge --helper, staged: install requested by uid 1000" in a, "no staged audit line: %r" % a[-1500:])


def record(box, text):
    """What a case records rather than assumes (pkexec's texts, the image's modes): one line in the suite's log."""
    print("RECORD : %s %s" % (box.name, text.replace("\n", " | ")), flush=True)


def staged_uninstall(box, remove_settings):
    exec_rule(box, "YES")
    res = window(box, "install", "--config", config_file(box, CONFIG_A))
    expect(res["rc"] == 0 and res["kind"] == "Done", "install: %r" % res)
    running_with(box, CONFIG_A["ports"][0]["tcpPort"])
    res = window(box, "uninstall", *(["--remove-settings"] if remove_settings else []))
    expect_no_stage_line(res, "uninstall")
    expect(res["rc"] == 0 and res["kind"] == "Done" and res["route"] == "staged" and res["op"] == "uninstall",
           "uninstall: %r" % res)
    expect(box.prop("LoadState") == "not-found", "the unit is still known: %s" % box.prop("LoadState"))
    for p in (UNIT_FILE, INSTALLED, POLICY, LEGACY_POLICY, "/etc/systemd/system/" + UNIT, sysd.LOCKDIR + "/engine.lock", ROOT):
        expect(not box.exists(p), "%s is still there after the staged uninstall" % p)
    a = audit(box)
    expect("aether-serial-bridge --helper, staged: uninstall requested by uid 1000" in a, "no staged audit line: %r" % a[-1500:])
    return res


@case("bootstrap_uninstall_keeps_settings")
def uninstall_keeps_settings(box):
    """The window's staged uninstall (the window's own build performs it, D62): service, program, policy, locks and
    the staging root gone; the settings kept."""
    staged_uninstall(box, False)
    expect(box.exists(DATA + "/config.json"), "the settings were removed without being asked")


@case("bootstrap_uninstall_remove_settings")
def uninstall_remove_settings(box):
    """The same with Also remove my settings: the settings folder goes too."""
    staged_uninstall(box, True)
    expect(not box.exists(DATA), "%s is still there" % DATA)


def tampered_program(box):
    put(box, "/tmp/tampered", "#!/bin/sh\ntouch /run/asb-ran\n", mode="755")
    return "/tmp/tampered"


@case("bootstrap_tampered_payload")
def tampered_payload(box):
    """INV-122: a payload that is not the program its digest names never runs: exit 3, "the program does not match its
    digest", no /run/asb-ran, nothing installed, no staging.* left. Also a real program with a wrong digest; and the
    root side under strace (run directly as root: pkexec is set-user-ID and cannot be traced): it never opens the
    user's file and never executes the staged copy."""
    exec_rule(box, "YES")
    real = sha_of(box, EXE)
    rows = [("tampered", tampered_program(box), real), ("wrong digest", EXE, "0" * 64)]
    for name, program, digest in rows:
        res = raw(box, program, {"op": "inspect"}, "--digest", digest)
        expect_stage_line(res, 3, "the program does not match its digest", name)
        expect(res["kind"] == "Refused", "%s: classified %s" % (name, res["kind"]))
        expect(not box.exists("/run/asb-ran"), "%s: the tampered payload ran" % name)
        nothing_installed(box, name)
        expect_root_clean(box, name)
    res = client(box, ["raw", "--program", "/tmp/tampered", "--request", request_file(box, {"op": "inspect"}), "--digest", real,
                       "--prefix", "/usr/bin/strace", "--prefix", "-f", "--prefix", "-qq", "--prefix", "-e",
                       "--prefix", "trace=execve,open,openat,creat", "--prefix", "-o", "--prefix", "/tmp/stage.trace"], user=None)
    expect_stage_line(res, 3, "the program does not match its digest", "under strace")
    trace = box.read("/tmp/stage.trace") or ""
    expect("execve(\"/bin/sh\"" in trace, "the trace has no root shell: %r" % trace[:800])
    expect("/tmp/tampered" not in trace, "the root side opened the user's file:\n%s" % "\n".join(
        l for l in trace.splitlines() if "/tmp/tampered" in l))
    staged_exec = [l for l in trace.splitlines() if "execve(\"" + ROOT in l]
    expect(not staged_exec, "the root side executed a staged copy: %r" % staged_exec)
    expect(not box.exists("/run/asb-ran"), "the tampered payload ran under strace")


@case("bootstrap_framing")
def framing(box):
    """INV-122: the framing is exact or nothing runs: each row exit 4 with its line, nothing installed, no staging.*
    left. The unterminated stream (more than declared, the writer keeping the pipe open) ends within 10 s: the read is
    bounded at the declared length plus one byte."""
    exec_rule(box, "YES")
    size = int(box.sh("stat -c %%s %s" % EXE).strip())
    req = {"op": "inspect"}
    rlen = len(json.dumps(req, separators=(",", ":"))) + 1
    rows = [
        ("short payload", ["--plen", str(size + 10)], "the payload is not %d+%d bytes" % (size + 10, rlen), None),
        ("long payload", ["--append-bytes", "5"], "the payload is not %d+%d bytes" % (size, rlen), None),
        ("unterminated, longer than declared", ["--append-bytes", "100000", "--hold-open-ms", "25000"],
         "the payload is not %d+%d bytes" % (size, rlen), 10.0),
        ("non-digit length", ["--plen", "12a"], "bad lengths", None),
        ("leading zero", ["--plen", "0" + str(size)], "bad lengths", None),
        ("empty request length", ["--rlen", ""], "bad lengths", None),
        ("program over 256 MiB", ["--plen", "268435457"], "bad lengths", None),
        ("request over 256 KiB", ["--rlen", "262145"], "bad lengths", None),
        ("65-character digest", ["--digest", "a" * 65], "bad digest", None),
        ("upper-case digest", ["--digest", "A" * 64], "bad digest", None),
        ("a path as an extra argument", ["--extra-arg", "/etc/shadow"], "bad arguments", None),
    ]
    for name, extra, line, within in rows:
        t0 = time.time()
        res = raw(box, EXE, req, *extra, timeout=120)
        took = time.time() - t0
        expect_stage_line(res, 4, line, name)
        expect(res["kind"] == "Refused", "%s: classified %s" % (name, res["kind"]))
        if within:
            expect(took < within, "%s: took %.1f s (an unbounded read waits for the writer)" % (name, took))
        nothing_installed(box, name)
        expect_root_clean(box, name, must_exist=False)


@case("bootstrap_unsafe_staging_root")
def unsafe_staging_root(box):
    """INV-122: the staging root must be a root:root 0700 directory (not a symlink), and every ancestor (/, /usr,
    /usr/local and /usr/local/lib) a directory only root can write; otherwise exit 4 naming what is wrong, nothing runs.
    A set-group-ID /usr/local/lib (2755) passes. The case restores every mode (/ is the case container's own)."""
    exec_rule(box, "YES")
    root_line = "the staging root is not a root-only directory"
    rows = [
        ("root is a symlink", "mkdir -p /tmp/elsewhere && ln -s /tmp/elsewhere %s" % ROOT, "rm -f %s" % ROOT, root_line),
        ("root 0755", "mkdir -m 0755 %s" % ROOT, "rmdir %s" % ROOT, root_line),
        ("root owned by tuser", "mkdir -m 0700 %s && chown tuser %s" % (ROOT, ROOT), "rmdir %s" % ROOT, root_line),
        ("/usr/local/lib root:staff 2775", "chgrp staff /usr/local/lib && chmod 2775 /usr/local/lib",
         "chgrp root /usr/local/lib && chmod 0755 /usr/local/lib && chmod g-s /usr/local/lib",
         "/usr/local/lib is not a directory only root can write"),
        ("/usr/local/lib 0757", "chmod 0757 /usr/local/lib", "chmod 0755 /usr/local/lib",
         "/usr/local/lib is not a directory only root can write"),
        ("/usr/local/lib owned by tuser", "chown tuser /usr/local/lib", "chown root /usr/local/lib",
         "/usr/local/lib is not a directory only root can write"),
        ("/usr/local 2775", "chmod 2775 /usr/local", "chmod 0755 /usr/local && chmod g-s /usr/local",
         "/usr/local is not a directory only root can write"),
        # Whoever can write / can rename /usr away after the check, as for any other ancestor.
        ("/ 0757", "chmod 0757 /", "chmod 0755 /", "/ is not a directory only root can write"),
    ]
    modes = box.sh("stat -c '%n %U:%G:%a' / /usr /usr/local /usr/local/lib")
    for name, setup, restore, line in rows:
        box.sh(setup)
        try:
            res = raw(box, EXE, {"op": "inspect"})
            expect_stage_line(res, 4, line, name)
            nothing_installed(box, name)
            if name == "root is a symlink":
                expect(box.sh("ls -A /tmp/elsewhere").strip() == "", "the symlink's target was written to")
            elif not name.startswith("root"):
                expect(not box.exists(ROOT), "%s: the staging root was created" % name)
        finally:
            box.sh(restore)
    expect(box.sh("stat -c '%n %U:%G:%a' / /usr /usr/local /usr/local/lib") == modes, "the modes were not restored")
    # A set-group-ID bit alone is fine, and stays fine: the staging root the first run creates under it is made
    # root:root 0700 (no inherited bit or group), so the second run passes the root check too.
    box.sh("chgrp staff /usr/local/lib && chmod 2755 /usr/local/lib")
    try:
        for run in ("first", "second"):
            res = raw(box, EXE, {"op": "inspect"})
            expect_no_stage_line(res, "setgid /usr/local/lib, %s run" % run)
            expect(res["rc"] == 0 and res["kind"] == "Done", "setgid /usr/local/lib, %s run: %r" % (run, res))
            expect_root_clean(box, "setgid /usr/local/lib, %s run" % run)
    finally:
        box.sh("chgrp root /usr/local/lib")
        box.sh("chmod 0755 /usr/local/lib && chmod g-s /usr/local/lib")
    expect_root_clean(box, "after the rows")


FIND_ROOT_SH = ("python3 -c \"import os\nfor p in os.listdir('/proc'):\n if p.isdigit():\n  try: a=open('/proc/%s/cmdline'%p,'rb')"
                ".read().split(b'\\0')\n  except OSError: continue\n  if len(a)>4 and a[0]==b'/bin/sh' and a[3]==b'asb-stage': print(p)\"")
FIND_STAGED = ("python3 -c \"import os\nfor p in os.listdir('/proc'):\n if p.isdigit():\n  try: a=open('/proc/%s/cmdline'%p,'rb')"
               ".read()\n  except OSError: continue\n  if b'/aether-serial-bridge/staging.' in a: print(p)\"")


def background_client(box, args):
    return box.bg("runuser -u tuser -- %s %s" % (CLIENT, " ".join(shlex.quote(a) for a in args)))


def wait_for_pid(box, finder, what, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        pids = box.run(finder).out.split()
        if pids:
            return pids[0]
        time.sleep(0.1)
    raise CaseFail("no %s appeared" % what)


def finished(proc, what, timeout=90):
    try:
        out, err = proc.communicate(timeout=timeout)
    except Exception:
        proc.kill()
        raise CaseFail("%s did not end" % what)
    lines = [l for l in out.decode(errors="replace").splitlines() if l.startswith("{\"")]
    expect(lines, "%s: no result (%r)" % (what, err.decode(errors="replace")[-800:]))
    return json.loads(lines[-1])


@case("bootstrap_signals")
def signals(box):
    """INV-122/124: (a) TERM to the root script while its bounded read waits, then the writer closes: exit 4,
    "interrupted", nothing ran, no staging.* left. (b) A stub staged with its own digest (sleep 3, then touch
    /run/asb-stub-ran, exit 1); TERM to the script during the sleep: the stub's own exit 1 passes through, no
    "interrupted" line, the stub finished, no staging.* left (a signal never reports "nothing was changed" over a
    change that ran)."""
    exec_rule(box, "YES")
    request_file(box, {"op": "inspect"})
    proc = background_client(box, ["raw", "--program", EXE, "--request", "/tmp/stage-request.json", "--send-bytes", "4096",
                                   "--hold-open-ms", "5000"])
    pid = wait_for_pid(box, FIND_ROOT_SH, "root staging shell")
    # The payload file is created by the bounded read's redirection, after both traps are set: only then is the
    # script waiting in the read (a TERM before its trap would just kill the shell).
    wait_for_pid(box, "ls %s/staging.*/payload 2>/dev/null && echo 1" % ROOT, "the bounded read")
    box.sh("kill -TERM %s" % pid)
    res = finished(proc, "(a)")
    expect_stage_line(res, 4, "interrupted", "(a) TERM during the read")
    nothing_installed(box, "(a)")
    expect_root_clean(box, "(a)")

    put(box, "/tmp/stub", "#!/bin/sh\nsleep 3\ntouch /run/asb-stub-ran\nexit 1\n", mode="755")
    proc = background_client(box, ["raw", "--program", "/tmp/stub", "--request", "/tmp/stage-request.json"])
    pid = wait_for_pid(box, FIND_ROOT_SH, "root staging shell")
    wait_for_pid(box, FIND_STAGED, "staged stub")
    box.sh("kill -TERM %s" % pid)
    res = finished(proc, "(b)")
    expect_no_stage_line(res, "(b) TERM during the program")
    expect(res["rc"] == 1, "(b): the stub's exit 1 did not pass through: %r" % res)
    expect(box.exists("/run/asb-stub-ran"), "(b): the stub did not finish")
    expect_root_clean(box, "(b)")


def root_tmpfs(box, opts):
    box.sh("mkdir -p -m 0700 %s && mount -t tmpfs -o mode=0700,uid=0,gid=0,%s tmpfs %s" % (ROOT, opts, ROOT))


@case("bootstrap_noexec_staging_is_not_cancel")
def noexec_staging(box):
    """INV-124: a root-only noexec tmpfs on the staging root: the verified copy cannot be executed; the script says so
    (exit 6, not the shell's 126, which pkexec uses for a cancel) and the window classifies it Refused."""
    exec_rule(box, "YES")
    root_tmpfs(box, "noexec")
    try:
        res = window(box, "install")
        expect_stage_line(res, 6, "the staged program could not be executed (126)", "noexec")
        expect(res["kind"] == "Refused" and "could not be started" in res["message"], "classified: %r" % res)
        nothing_installed(box, "noexec")
        expect_root_clean(box, "noexec")
    finally:
        box.run("umount %s" % ROOT)


@case("bootstrap_full_staging_root")
def full_staging_root(box):
    """INV-124: a root-only 1 MiB tmpfs on the staging root (the payload does not fit): exit 4, "cannot store the
    payload", Refused "Nothing was changed", never StateUnknown."""
    exec_rule(box, "YES")
    root_tmpfs(box, "size=1M")
    try:
        res = window(box, "install", "--config", config_file(box, CONFIG_A))
        expect_stage_line(res, 4, "cannot store the payload", "full")
        expect(res["kind"] == "Refused" and res["message"] == "Nothing was changed: cannot store the payload.",
               "classified: %r" % res)
        nothing_installed(box, "full")
        expect_root_clean(box, "full")
    finally:
        box.run("umount %s" % ROOT)


@case("bootstrap_not_authorized")
def not_authorized(box):
    """INV-124: a NO rule: pkexec's 127, its stderr recorded, no asb-stage line, NotAuthorized, nothing changed."""
    exec_rule(box, "NO")
    res = window(box, "install", "--config", config_file(box, CONFIG_A))
    record(box, "pkexec under a NO rule: exit %s, stderr %r" % (res["rc"], res["err"]))
    expect_no_stage_line(res, "NO rule")
    expect(res["rc"] == 127 and res["kind"] == "NotAuthorized", "NO rule: %r" % res)
    expect("Is the bridge installed" not in res["message"] and "--install" in res["message"], "the message: %r" % res["message"])
    nothing_installed(box, "NO rule")
    expect(not box.exists(ROOT), "a staging root was created")


@case("bootstrap_no_agent")
def no_agent(box):
    """INV-124: no rule (polkit's defaults: auth_admin), no agent and no terminal: what pkexec returns is recorded; no
    asb-stage line; classified NoAgent when its stderr says no agent could ask; nothing changed."""
    no_rule(box)
    res = window(box, "install")
    record(box, "pkexec with no rule and no agent: exit %s, stderr %r" % (res["rc"], res["err"]))
    expect_no_stage_line(res, "no agent")
    expect(res["rc"] == 127, "no agent: %r" % res)
    says_no_agent = "No authentication agent" in res["err"] or "Error creating textual authentication agent" in res["err"]
    expect(res["kind"] == ("NoAgent" if says_no_agent else "NotAuthorized"), "classified %s for %r" % (res["kind"], res["err"]))
    nothing_installed(box, "no agent")


def with_agent(box, answer_args, client_args, timeout=300):
    box.write("/tmp/pty_agent.py", (HERE / "pty_agent.py").read_text())
    box.sh("chmod 644 /tmp/pty_agent.py")
    cmd = "python3 /tmp/pty_agent.py %s -- %s %s --gate" % (" ".join(answer_args), CLIENT, " ".join(shlex.quote(a) for a in client_args))
    r = box.run(cmd, user="tuser", timeout=timeout)
    lines = [l for l in r.out.splitlines() if l.startswith("{\"")]
    expect(r.rc == 0 and lines, "pty agent: rc=%s out=%r err=%r" % (r.rc, r.out[-2000:], r.err[-1500:]))
    prompts = [int(l.split()[1]) for l in r.out.splitlines() if l.startswith("PROMPTS ")]
    pty_lines = [l for l in r.out.splitlines() if l.startswith("PTY: ")]
    return json.loads(lines[-1]), (prompts or [0])[0], pty_lines


@case("bootstrap_auth_admin_pkttyagent")
def auth_admin_pkttyagent(box):
    """A real auth_admin prompt (no rule: polkit's own default for the generic exec action), answered on a
    pseudo-terminal by pkttyagent with the test administrator's password: the staged inspect returns its response, no
    asb-stage line. Then wrong answers: 127, NotAuthorized, nothing ran."""
    no_rule(box)
    request_file(box, {"op": "inspect"})
    args = ["raw", "--program", EXE, "--request", "/tmp/stage-request.json"]
    res, prompts, pty = with_agent(box, ["--answer", ADMIN_PASSWORD], args)
    record(box, "pkttyagent, right password: %d prompt(s), %s; exit %s" % (prompts, pty, res["rc"]))
    expect_no_stage_line(res, "right password")
    expect(prompts >= 1 and res["rc"] == 0 and res["kind"] == "Done", "right password: %r" % res)
    expect(json.loads(res["out"])["installation"] == "NotInstalled", "the staged inspect's response: %r" % res["out"])
    res, prompts, pty = with_agent(box, ["--answer", "not-the-password"], args)
    record(box, "pkttyagent, wrong password: %d prompt(s), %s; exit %s, stderr %r" % (prompts, pty, res["rc"], res["err"]))
    expect_no_stage_line(res, "wrong password")
    expect(prompts >= 1 and res["rc"] == 127 and res["kind"] == "NotAuthorized", "wrong password: %r" % res)
    expect_root_clean(box, "after the prompts")


@case("bootstrap_cancel_with_queued_payload")
def cancel_with_queued_payload(box):
    """The prompt ended at the terminal (EOF at "Password:") while the whole program is still queued on pkexec's stdin:
    the code is recorded (pkttyagent reports EOF as a failed authentication; a desktop agent's Cancel is 126, hand
    test L-5), no asb-stage line, the launcher survives the closed pipe, nothing changed."""
    no_rule(box)
    res, prompts, pty = with_agent(box, ["--eof"], ["window", "install", "--program", EXE])
    record(box, "pkttyagent EOF at the prompt: %d prompt(s), %s; exit %s, stderr %r" % (prompts, pty, res["rc"], res["err"]))
    expect_no_stage_line(res, "EOF")
    expect(prompts >= 1 and res["rc"] in (126, 127), "EOF at the prompt: %r" % res)
    expect(res["kind"] == ("Cancelled" if res["rc"] == 126 else "NotAuthorized"), "classified %r" % res)
    nothing_installed(box, "cancel")
    expect(not box.exists(ROOT), "a staging root was created")


@case("polkit_exec_defaults_bookworm")
def exec_defaults_bookworm(box):
    """R21, R28: what Debian's polkit does with the generic exec action, asserted (a change fails loudly): implicit
    any/inactive/active = auth_admin, the admin rule naming unix-group:sudo; /, /usr, /usr/local and /usr/local/lib
    root-owned with no group or other write bit (otherwise every bootstrap case would exit 4: this case names why)."""
    r = box.run("pkaction --verbose --action-id %s" % EXEC_ACTION)
    record(box, "pkaction: %s" % r.out.strip())
    for line in ("implicit any:      auth_admin", "implicit inactive: auth_admin", "implicit active:   auth_admin"):
        expect(line in r.out, "pkaction: %r missing in %r" % (line, r.out))
    rules = box.run("cat /usr/share/polkit-1/rules.d/*.rules /etc/polkit-1/rules.d/*.rules 2>/dev/null").out
    expect("addAdminRule" in rules and "unix-group:sudo" in rules, "no admin rule naming unix-group:sudo: %r" % rules[-1500:])
    modes = box.sh("stat -c '%n %U:%G:%a' / /usr /usr/local /usr/local/lib")
    record(box, "modes: %s" % modes.strip())
    for line in modes.strip().splitlines():
        path, owner, mode = line.split()[0], line.split()[1].rsplit(":", 1)[0], line.split()[1].rsplit(":", 1)[1]
        expect(owner.startswith("root:"), "%s is owned by %s" % (path, owner))
        expect(int(mode[-2]) & 2 == 0 and int(mode[-1]) & 2 == 0, "%s is group- or other-writable (%s)" % (path, mode))


# --- the installed-program cases: a real v1.0 program (Decision 10), and this build installed --------------------------
V1_OPS = {"save-and-apply", "deactivate", "start", "stop", "repair", "discard-pending", "inspect"}
FIXTURE_NAMES = {"x86_64": ("aether-serial-bridge-linux-x64", "V10_SHA256_X64"),
                 "aarch64": ("aether-serial-bridge-linux-arm64", "V10_SHA256_ARM64")}


def v10_fixture(box):
    """The pinned v1.0 program for this machine (tests/systemd/fixtures.env). SKIP when it is missing or not pinned,
    FAIL when it is not the pinned file. ASB_V10_FIXTURE_SHA256 (a developer's local stand-in of the same commit) is
    recorded as such; CI never sets it."""
    pins = dict(l.split("=", 1) for l in (HERE / "fixtures.env").read_text().splitlines() if "=" in l and not l.startswith("#"))
    machine = box.sh("uname -m").strip()
    if machine not in FIXTURE_NAMES:
        raise CaseSkip("no v1.0 fixture is defined for %s" % machine)
    name, key = FIXTURE_NAMES[machine]
    path = "%s/v1.0/%s" % (sysd.FIXTURES, name)
    if not box.exists(path):
        raise CaseSkip("no v1.0 fixture mounted at %s (driver.py --fixtures; release.yml builds it, Decision 10)" % path)
    local = os.environ.get("ASB_V10_FIXTURE_SHA256", "")
    pin = local or pins.get(key, "").strip()
    if not pin:
        raise CaseSkip("the v1.0 fixture is not pinned yet (%s in tests/systemd/fixtures.env)" % key)
    got = sha_of(box, path)
    expect(got == pin, "the v1.0 fixture is %s, pinned %s" % (got, pin))
    if local:
        record(box, "v1.0 fixture: a LOCAL stand-in of %s (%s), not the pinned release build" % (pins.get("V10_COMMIT", "?")[:12], got))
    return path


def install_v10(box):
    fixture = v10_fixture(box)
    r = box.run("%s --install" % fixture)
    expect(r.rc == 0, "the v1.0 program's own --install: %r" % r)
    expect(sha_of(box, INSTALLED) == sha_of(box, fixture), "--install did not install the v1.0 program")
    return fixture


def known_actions(box):
    """The bridge's actions polkitd lists now (it reloads /usr/share/polkit-1/actions on change)."""
    return {a for a in box.run("pkaction").out.split() if a.endswith(".aetherserialbridge.helper")}


def expect_only_current_policy(box, what):
    """This build's policy installed, the v1.0 one gone, and polkitd lists only this build's action."""
    expect(box.exists(POLICY), "%s: no %s" % (what, POLICY))
    expect(not box.exists(LEGACY_POLICY), "%s: %s is still there" % (what, LEGACY_POLICY))
    box.wait_until(lambda: known_actions(box) == {HELPER_ACTION}, timeout=30,
                   what="%s: polkitd to list %s only (it lists %r)" % (what, HELPER_ACTION, known_actions(box)))


def as_root_helper(box, program, req):
    path = request_file(box, req, "/tmp/root-request.json")
    r = box.run("%s --helper < %s" % (program, path))
    expect(r.rc in (0, 1), "%s --helper %s: %r" % (program, req.get("op"), r))
    return json.loads(r.out)


@case("bootstrap_update_v10_fixture")
def update_v10_fixture(box):
    """A real v1.0 program installed by its own --install and running settings A -> the window's staged install (the
    update): the program is replaced by this build byte for byte, the settings are kept (A, still running); then the
    update with the editor's ports B in one prompt: B runs. The new program's --status then reads it."""
    install_v10(box)
    expect(box.exists(LEGACY_POLICY) and not box.exists(POLICY), "the v1.0 program did not install the v1.0 policy alone")
    a = json.dumps(CONFIG_A, separators=(",", ":")).encode()
    res = as_root_helper(box, INSTALLED, {"op": "save-and-apply", "configBytes": base64.b64encode(a).decode(),
                                          "source": {"kind": "new"}, "supersedePending": True})
    expect(res["outcome"] == "Ok", "the v1.0 helper's save-and-apply: %r" % res)
    running_with(box, CONFIG_A["ports"][0]["tcpPort"])
    exec_rule(box, "YES")
    res = window(box, "update")
    expect_no_stage_line(res, "update")
    expect(res["rc"] == 0 and res["kind"] == "Done" and res["route"] == "staged", "update: %r" % res)
    expect(sha_of(box, INSTALLED) == sha_of(box, EXE), "the program was not replaced by this build")
    expect(box.run("cat %s/config.json" % DATA).out.encode() == a, "the settings were not kept")
    running_with(box, CONFIG_A["ports"][0]["tcpPort"])
    expect_only_current_policy(box, "after the update")
    res = window(box, "update", "--config", config_file(box, CONFIG_B))
    expect_no_stage_line(res, "update with ports")
    expect(res["rc"] == 0 and res["kind"] == "Done" and res["clearsDirty"], "update with ports: %r" % res)
    running_with(box, CONFIG_B["ports"][0]["tcpPort"])
    r = box.run("%s --status" % INSTALLED)
    expect(r.rc == 0 and "Service: running" in r.out, "--status of the new program: %r" % r)
    expect_root_clean(box, "after the updates")


@case("bootstrap_uninstall_v10_fixture")
def uninstall_v10_fixture(box):
    """The upgrade path for an installation made before the identifiers moved to org.aethersdr (2026-10-22): a real
    v1.0 program installed by its own --install (the agency.cloaked policy) -> this window's staged uninstall removes
    the service, the program and that policy; then this window's staged install puts this build's policy alone."""
    install_v10(box)
    expect(box.exists(LEGACY_POLICY), "the v1.0 program installed no v1.0 policy")
    exec_rule(box, "YES")
    res = window(box, "uninstall")
    expect_no_stage_line(res, "uninstall")
    expect(res["rc"] == 0 and res["kind"] == "Done" and res["route"] == "staged" and res["op"] == "uninstall",
           "uninstall of the v1.0 installation: %r" % res)
    nothing_installed(box, "after the uninstall")
    box.wait_until(lambda: not known_actions(box), timeout=30,
                   what="polkitd to drop the bridge's action (it lists %r)" % known_actions(box))
    res = window(box, "install", "--config", config_file(box, CONFIG_A))
    expect(res["rc"] == 0 and res["kind"] == "Done", "the reinstall: %r" % res)
    running_with(box, CONFIG_A["ports"][0]["tcpPort"])
    expect_only_current_policy(box, "after the reinstall")
    expect_root_clean(box, "after the reinstall")


@case("v10_installed_day_to_day")
def v10_installed_day_to_day(box):
    """INV-123: with a real v1.0 program installed, the window's day-to-day actions (Save & apply on a kept
    installation, Stop, Start, the untick, Discard) go to the installed helper with v1.0 ops only (Discard is v1.0's
    discard-pending: the program has no build stamp), and each works under the v1.0 policy's action. The v1.0 program
    then reads the settings this window saved (WS-10 debt)."""
    install_v10(box)
    # The bridge's own action only (a staged request would be refused), as the v1.0 policy names it.
    helper_rule(box, "YES", LEGACY_HELPER_ACTION)
    seen = []

    def act(action, *extra, want=("Ok",)):
        res = window(box, action, *extra)
        expect_no_stage_line(res, action)
        expect(res["route"] == "installed" and res["op"] in V1_OPS, "%s: sent %s on the %s route" % (action, res["op"], res["route"]))
        expect(res["rc"] in (0, 1) and res["kind"] == "Done", "%s: %r" % (action, res))
        out = json.loads(res["out"])
        expect(out["outcome"] in want, "%s: %s (%s)" % (action, out["outcome"], out["message"]))
        seen.append(res["op"])
        return res

    act("apply", "--config", config_file(box, CONFIG_A))
    running_with(box, CONFIG_A["ports"][0]["tcpPort"])
    act("stop")
    box.wait_until(lambda: box.active() in ("inactive", "failed"), timeout=30, what="the stop")
    act("start")
    running_with(box, CONFIG_A["ports"][0]["tcpPort"])
    act("untick", "--config", config_file(box, CONFIG_B, "/tmp/stage-config-b.json"), want=("SavedNotApplied",))
    expect(box.exists(DATA + "/config.pending.json"), "the untick kept no pending settings")
    pending_sha = sha_of(box, DATA + "/config.pending.json")
    act("discard", "--pending-sha", pending_sha)
    expect(not box.exists(DATA + "/config.pending.json"), "Discard left the pending settings")
    expect(seen == ["save-and-apply", "stop", "start", "deactivate", "discard-pending"], "the ops sent: %r" % seen)
    r = box.run("%s --show-config" % INSTALLED)
    expect(r.rc == 0 and CONFIG_A["ports"][0]["comPort"] in r.out and str(CONFIG_A["ports"][0]["tcpPort"]) in r.out,
           "the v1.0 program cannot read the settings this window saved: %r" % r)
    nothing_staged = not box.exists(ROOT)
    expect(nothing_staged, "a staging root was created by a day-to-day action")


@case("v11_installed_helper_refuses_lifecycle_ops")
def v11_installed_refuses_lifecycle(box):
    """INV-123: this build installed through the staged route; its INSTALLED helper (pkexec, the bridge's own
    action) refuses install, with and without settings, and uninstall: exit 2, the refusal line, nothing on stdout; the
    unit, the program, the settings and the policy unchanged, the service still running. The same helper runs
    discard-pending-if (the window sends it: the program carries a build stamp)."""
    exec_rule(box, "YES", helper_answer="YES")
    res = window(box, "install", "--config", config_file(box, CONFIG_A))
    expect(res["rc"] == 0 and res["kind"] == "Done", "install: %r" % res)
    running_with(box, CONFIG_A["ports"][0]["tcpPort"])

    def snapshot():
        return [sha_of(box, p) for p in (INSTALLED, UNIT_FILE, POLICY, DATA + "/config.json")] + [box.active()]

    before = snapshot()
    b = base64.b64encode(json.dumps(CONFIG_B).encode()).decode()
    for req in ({"op": "install"}, {"op": "install", "configBytes": b, "source": {"kind": "new"}, "supersedePending": True},
                {"op": "uninstall", "removeSettings": False}, {"op": "uninstall", "removeSettings": True}):
        path = request_file(box, req, "/tmp/installed-request.json")
        r = box.run("pkexec %s --helper < %s" % (INSTALLED, path), user="tuser")
        expect(r.rc == 2 and r.out == "" and REFUSED_LIFECYCLE in r.err, "%s: rc=%s out=%r err=%r" % (req, r.rc, r.out, r.err))
        expect(snapshot() == before, "%s through the installed helper changed something" % req)
    box.write(DATA + "/config.pending.json", json.dumps(CONFIG_B))
    box.sh("chmod 0644 %s/config.pending.json" % DATA)
    res = window(box, "discard", "--pending-sha", sha_of(box, DATA + "/config.pending.json"))
    expect_no_stage_line(res, "discard")
    expect(res["route"] == "installed" and res["op"] == "discard-pending-if" and res["rc"] == 0 and res["kind"] == "Done",
           "discard on a v1.1 installation: %r" % res)
    expect(not box.exists(DATA + "/config.pending.json"), "discard-pending-if left the pending settings")


# --- another distribution's polkit (R21): Fedora, in a PolkitBox (no systemd) ------------------------------------------
@case("polkit_exec_defaults_fedora", lockdir=False, kind="fedora")
def exec_defaults_fedora(box):
    """R21, R28 on Fedora: the generic exec action is auth_admin in every session state, the admin rule names
    unix-group:wheel, and /, /usr, /usr/local and /usr/local/lib are root's alone (asserted, recorded)."""
    r = box.run("pkaction --verbose --action-id %s" % EXEC_ACTION)
    record(box, "pkaction: %s; polkit %s" % (r.out.strip(), box.run("rpm -q polkit").out.strip()))
    for line in ("implicit any:      auth_admin", "implicit inactive: auth_admin", "implicit active:   auth_admin"):
        expect(line in r.out, "pkaction: %r missing in %r" % (line, r.out))
    rules = box.run("cat /usr/share/polkit-1/rules.d/*.rules /etc/polkit-1/rules.d/*.rules 2>/dev/null").out
    expect("addAdminRule" in rules and "unix-group:wheel" in rules, "no admin rule naming unix-group:wheel: %r" % rules[-1500:])
    modes = box.sh("stat -c '%n %U:%G:%a' / /usr /usr/local /usr/local/lib")
    record(box, "modes: %s" % modes.strip())
    for line in modes.strip().splitlines():
        path, owner, mode = line.split()[0], line.split()[1].rsplit(":", 1)[0], line.split()[1].rsplit(":", 1)[1]
        expect(owner.startswith("root:"), "%s is owned by %s" % (path, owner))
        expect(int(mode[-2]) & 2 == 0 and int(mode[-1]) & 2 == 0, "%s is group- or other-writable (%s)" % (path, mode))


@case("bootstrap_auth_admin_fedora", lockdir=False, kind="fedora")
def auth_admin_fedora(box):
    """The staged inspect through Fedora's pkexec with a real auth_admin prompt answered by pkttyagent as a wheel
    administrator: the script stages and verifies the program and its response comes back. There is no systemd here:
    with no unit file the program reports NotInstalled / Absent without asking it (recorded 2026-10-17; the plan
    guessed "Unknown"). No asb-stage line."""
    request_file(box, {"op": "inspect"})
    res, prompts, pty = with_agent(box, ["--answer", ADMIN_PASSWORD],
                                   ["raw", "--program", EXE, "--request", "/tmp/stage-request.json"])
    record(box, "Fedora pkttyagent: %d prompt(s), %s; exit %s, stderr %r" % (prompts, pty, res["rc"], res["err"][-600:]))
    expect_no_stage_line(res, "Fedora")
    expect(prompts >= 1 and res["rc"] in (0, 1) and res["kind"] == "Done", "Fedora staged inspect: %r" % res)
    report = json.loads(res["out"])
    record(box, "Fedora staged inspect: %r" % report)
    expect(report["installation"] == "NotInstalled" and report["service"] == "Absent", "the inspect without systemd: %r" % report)
    expect_root_clean(box, "Fedora")
