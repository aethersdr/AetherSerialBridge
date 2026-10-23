"""The pkexec helper (WS-9b, master 5.7): root only (INV-50), deactivate keeps helper, unit and policy (INV-51), the
bounded request (INV-89: a 200 MB flood under an address-space limit, no EOF, two values), the polkit policy as
installed, and the GUI client's round trip against the installed program (tst_HelperRoundTrip: directly as root,
and through pkexec under a test polkit rule)."""

import base64
import hashlib
import json
import pathlib

import sysd

from sysd import SEAM_DIR, BIN, CONFIG_A, CONFIG_B, DATA, EXE, INSTALLED, UNIT, CaseFail, case, expect, running_with

POLICY = "/usr/share/polkit-1/actions/org.aethersdr.aetherserialbridge.policy"
ACTION = "org.aethersdr.aetherserialbridge.helper"
LEGACY_POLICY = "/usr/share/polkit-1/actions/agency.cloaked.aetherserialbridge.policy"   # up to v1.1.0
APPLY_LOCK = "/run/aether-serial-bridge.apply.lock"
REPO = pathlib.Path(__file__).resolve().parents[3]
REFUSED_ROOT = "--helper runs only as root, started by pkexec; nothing was changed."


def _bytes(config):
    return json.dumps(config, separators=(",", ":")).encode()


def request(op, config=None, kind="new", sha=None, supersede=True):
    r = {"op": op}
    if config is not None:
        r["configBytes"] = base64.b64encode(config).decode()
        r["source"] = {"kind": kind}
        if sha:
            r["source"]["sha256"] = sha
        r["supersedePending"] = supersede
    return json.dumps(r)


def helper(box, text, program=INSTALLED, user=None, env=None, timeout=120):
    """Runs `program --helper` with `text` on its stdin (then EOF): (rc, parsed response or None, stdout, stderr)."""
    box.write("/tmp/helper-request", text)
    box.sh("chmod 644 /tmp/helper-request")
    r = box.run("%s --helper < /tmp/helper-request" % program, user=user, env=env, timeout=timeout)
    res = None
    if r.out.strip():
        try:
            res = json.loads(r.out)
        except ValueError:
            pass
    return r.rc, res, r.out, r.err


def _ok(r, what):
    expect(r.rc == 0, "%s: rc=%s out=%r err=%r" % (what, r.rc, r.out[-1500:], r.err[-1500:]))


def timed(box, script, timeout=120):
    """Runs a bash script that prints "<rc> <milliseconds>" on its last line; times are measured inside the container
    (docker exec's own start-up would otherwise count)."""
    r = box.run(script, timeout=timeout)
    last = r.out.strip().splitlines()[-1] if r.out.strip() else ""
    try:
        rc, ms = (int(x) for x in last.split())
    except ValueError:
        raise CaseFail("timing script printed %r (err %r)" % (r.out, r.err))
    return rc, ms, r


@case("helper_requires_euid0")
def requires_euid0(box):
    """INV-50: the helper runs only as root. A user with PKEXEC_UID=0 in its environment (what pkexec would set) is
    refused before anything is read or created: exit 2, the refusal, nothing on stdout, no data folder, no lock. Root
    with the same request is the positive control."""
    for env in ({"PKEXEC_UID": "0"}, {"PKEXEC_UID": "1000"}, {}):
        rc, res, out, err = helper(box, request("save-and-apply", _bytes(CONFIG_A)), program=EXE, user="tuser", env=env)
        expect(rc == 2 and out == "" and REFUSED_ROOT in err, "as tuser with %s: rc=%s out=%r err=%r" % (env, rc, out, err))
    expect(not box.exists(DATA) and not box.exists(APPLY_LOCK) and not box.exists(INSTALLED),
           "the refused helper created something")
    _ok(box.cli("--install"), "--install")
    rc, res, out, err = helper(box, request("stop"), user="tuser", env={"PKEXEC_UID": "0"})
    expect(rc == 2 and out == "" and REFUSED_ROOT in err, "installed, as tuser: rc=%s out=%r err=%r" % (rc, out, err))
    expect(box.active() == "active", "the refused stop stopped the service")
    rc, res, out, err = helper(box, request("stop"))
    expect(rc == 0 and res and res["ok"] and res["outcome"] == "Ok", "as root: rc=%s out=%r err=%r" % (rc, out, err))
    box.wait_until(lambda: box.active() in ("inactive", "failed"), timeout=30, what="the stop")
    rc, res, out, err = helper(box, request("start"))
    expect(rc == 0 and res and res["outcome"] == "Ok", "start as root: rc=%s out=%r err=%r" % (rc, out, err))
    expect(box.active() == "active", "not active after the helper's start")


@case("deactivate_keeps_helper")
def deactivate_keeps_helper(box):
    """INV-51: untick (deactivate) keeps the helper binary, the unit and the polkit policy; the service is masked and
    stopped; committed stays A and the edits B are pending; the helper still answers. Only --uninstall removes them."""
    _ok(box.cli("--install"), "--install")
    a = _bytes(CONFIG_A)
    rc, res, out, err = helper(box, request("save-and-apply", a))
    expect(rc == 0 and res["outcome"] == "Ok", "save-and-apply A: rc=%s out=%r err=%r" % (rc, out, err))
    running_with(box, CONFIG_A["ports"][0]["tcpPort"])
    b = json.dumps(CONFIG_B, indent=2).encode()
    rc, res, out, err = helper(box, request("deactivate", b, kind="committed", sha=hashlib.sha256(a).hexdigest(),
                                            supersede=False))
    expect(rc == 0 and res["outcome"] == "SavedNotApplied" and res["persisted"] == "Staged" and
           res["persistedSha256"] == hashlib.sha256(b).hexdigest(), "deactivate: rc=%s out=%r err=%r" % (rc, out, err))
    for path in (INSTALLED, "/usr/local/lib/systemd/system/" + UNIT, POLICY):
        expect(box.exists(path), "deactivate removed %s" % path)
    expect(box.prop("LoadState") == "masked", "not masked after deactivate: %s" % box.prop("LoadState"))
    expect(box.active() in ("inactive", "failed"), "still running after deactivate: %s" % box.active())
    expect(box.run("cat %s/config.json" % DATA).out.encode() == a, "committed is not A")
    expect(box.run("cat %s/config.pending.json" % DATA).out.encode() == b, "pending is not B")
    rc, res, out, err = helper(box, request("inspect"))
    expect(rc == 0 and res and res["trust"] == "Trust" and res["installation"] == "Current" and
           res["service"] != "Running", "inspect after deactivate: rc=%s out=%r err=%r" % (rc, out, err))
    _ok(box.cli("--uninstall"), "--uninstall")
    for path in (INSTALLED, "/usr/local/lib/systemd/system/" + UNIT, POLICY):
        expect(not box.exists(path), "--uninstall left %s" % path)


@case("helper_flood_rejected")
def flood_rejected(box):
    """INV-89: a 200 MB stream into the helper under a 128 MiB address-space limit is refused within 1 s with exit 2
    "too large" (the cumulative cap: nothing past 256 KiB + 1 is read or held); the running service is untouched."""
    _ok(box.cli("--install"), "--install")
    rc, ms, r = timed(box, "s=$(date +%%s%%N); head -c 209715200 /dev/zero | prlimit --as=134217728 %s --helper "
                           ">/tmp/flood.out 2>/tmp/flood.err; rc=${PIPESTATUS[1]}; e=$(date +%%s%%N); "
                           "echo $rc $(( (e - s) / 1000000 ))" % INSTALLED)
    err, out = box.read("/tmp/flood.err") or "", box.read("/tmp/flood.out") or ""
    expect(rc == 2 and "the request is too large (more than 262144 bytes); nothing was changed." in err,
           "flood: rc=%s err=%r" % (rc, err[-1500:]))
    expect(out == "", "the flood got a response: %r" % out[:200])
    expect(ms < 1000, "the flood took %d ms (more than 1 s)" % ms)
    expect(box.active() == "active", "the flood disturbed the service")
    # A valid request of the same program under the same limit works: the limit is not what refused the flood.
    box.write("/tmp/inspect.json", request("inspect"))
    r = box.run("prlimit --as=134217728 %s --helper < /tmp/inspect.json" % INSTALLED)
    expect(r.rc == 0 and json.loads(r.out)["installation"] == "Current", "inspect under the limit: %r" % r)


@case("helper_no_eof_deadline")
def no_eof_deadline(box):
    """INV-89: one 10 s deadline for the whole request, enforced without blocking: an input that stays open is refused
    at 10 s (exit 2), and so is one that keeps trickling a byte every 0.5 s (the deadline is cumulative, not per read).
    Nothing changes."""
    _ok(box.cli("--install"), "--install")
    rows = {
        "silent": "(printf '{\"op\":\"stop\"'; sleep 40)",
        "trickle": "(for i in $(seq 1 80); do printf ' '; sleep 0.5; done)",
    }
    for name, feeder in rows.items():
        rc, ms, r = timed(box, "exec 3< <(%s); s=$(date +%%s%%N); %s --helper <&3 >/tmp/late.out 2>/tmp/late.err; rc=$?; "
                               "e=$(date +%%s%%N); exec 3<&-; echo $rc $(( (e - s) / 1000000 ))" % (feeder, INSTALLED),
                          timeout=90)
        err = box.read("/tmp/late.err") or ""
        expect(rc == 2 and "the request did not end within 10 s; nothing was changed." in err,
               "%s: rc=%s err=%r" % (name, rc, err[-800:]))
        expect(9500 <= ms <= 12500, "%s: refused after %d ms, not at the 10 s deadline" % (name, ms))
        expect((box.read("/tmp/late.out") or "") == "", "%s: a response was written" % name)
        expect(box.active() == "active", "%s: the service was disturbed" % name)


@case("helper_two_values_rejected")
def two_values_rejected(box):
    """INV-89: exactly one JSON value. Two values (or a value and anything but whitespace) are refused with exit 2 and
    nothing on stdout, and the first one is not run: the service keeps running. One value with trailing whitespace is
    the positive control (it stops the service)."""
    _ok(box.cli("--install"), "--install")
    for text in ('{"op":"stop"}{"op":"start"}', '{"op":"stop"}\n{"op":"stop"}\n', '{"op":"stop"} x',
                 '{"op":"stop","op":"start"}'):
        rc, res, out, err = helper(box, text)
        expect(rc == 2 and out == "" and "the request is not valid" in err, "%r: rc=%s out=%r err=%r" % (text, rc, out, err))
        expect(box.active() == "active", "%r: the service was stopped by a refused request" % text)
    rc, res, out, err = helper(box, '{"op":"stop"} \n\t\r\n')
    expect(rc == 0 and res and res["outcome"] == "Ok", "one value + whitespace: rc=%s out=%r err=%r" % (rc, out, err))
    box.wait_until(lambda: box.active() in ("inactive", "failed"), timeout=30, what="the stop")


@case("helper_policy_installed")
def policy_installed(box):
    """WS-9b: --install writes the polkit policy byte for byte (root:root 0644), polkitd lists the action with
    auth_admin and the exec.path of the installed program; --uninstall removes it. The agency.cloaked policy of the
    releases up to v1.1.0 (same exec.path) is removed by --install and, put back, by --uninstall."""
    want = (REPO / "packaging" / "linux" / "org.aethersdr.aetherserialbridge.policy").read_bytes()
    legacy = want.decode().replace(ACTION, "agency.cloaked.aetherserialbridge.helper")
    box.write(LEGACY_POLICY, legacy, mode="644")
    _ok(box.cli("--install"), "--install")
    expect(not box.exists(LEGACY_POLICY), "--install left the agency.cloaked policy")
    got = box.run("base64 -w0 %s" % POLICY).out
    expect(base64.b64decode(got) == want, "the installed policy differs from packaging/linux")
    expect(box.sh("stat -c '%%U:%%G %%a' %s" % POLICY).strip() == "root:root 644", "policy owner/mode")
    r = box.run("pkaction --action-id %s --verbose" % ACTION)
    expect(r.rc == 0, "pkaction does not know the action: %r" % r)
    for line in ("implicit any:      auth_admin", "implicit inactive: auth_admin", "implicit active:   auth_admin",
                 "org.freedesktop.policykit.exec.path -> /usr/local/bin/aether-serial-bridge",
                 "org.freedesktop.policykit.exec.argv1 -> --helper"):
        expect(line in r.out, "pkaction: %r missing in %r" % (line, r.out))
    box.write(LEGACY_POLICY, legacy, mode="644")
    _ok(box.cli("--uninstall"), "--uninstall")
    expect(not box.exists(POLICY), "--uninstall left the policy")
    expect(not box.exists(LEGACY_POLICY), "--uninstall left the agency.cloaked policy")


_RULE = """polkit.addRule(function(action, subject) {
    if (action.id == "%s" && subject.user == "tuser") return polkit.Result.%s;
});
"""


def _rule(box, answer):
    box.write("/etc/polkit-1/rules.d/50-asb-test.rules", _RULE % (ACTION, answer))
    box.sh("chmod 644 /etc/polkit-1/rules.d/50-asb-test.rules && systemctl restart polkit")


def _roundtrip(box, mode, user=None, env=None):
    e = {"ASB_HELPER_ROUNDTRIP": mode}
    e.update(env or {})
    # Release mode: the installed program is the release binary, which has no failure seam; the one function that
    # stages a failure through it runs in the seam-build pass.
    only = "" if not sysd.RELEASE else " whitespaceVariantsAreTwoDigestsEchoedExactly digestMismatchKeepsDirty " \
        "inspectRoundTrip deniedByPolicyChangesNothing"
    r = box.run("%s/tst_HelperRoundTrip -o -,txt%s" % (SEAM_DIR, only), user=user, env=e, timeout=600)
    expect(r.rc == 0 and "0 failed" in r.out, "tst_HelperRoundTrip (%s): rc=%s\n%s\n%s" % (mode, r.rc, r.out[-4000:], r.err[-2000:]))
    return r.out


@case("helper_round_trip")
def round_trip(box):
    """tst_HelperRoundTrip against the installed program: (1) whitespace variants echo two exact digests, config.json
    holds the bytes sent; (2) a staged failure clears dirty only for its digest (directly as root: the failure seam is
    an environment variable, which pkexec clears); (3) a digest mismatch keeps dirty. Then through pkexec as an
    ordinary user under a YES rule for the helper's action (the installed policy maps the program to it; the audit line
    names the user's uid), and under a NO rule: not authorized, nothing changed."""
    _ok(box.cli("--install"), "--install")
    out = _roundtrip(box, "direct")
    for name in ("whitespaceVariantsAreTwoDigestsEchoedExactly", "digestMismatchKeepsDirty", "inspectRoundTrip") + (
            () if sysd.RELEASE else ("stagedFailureClearsDirtyOnlyForTheMatchingDigest",)):
        expect("PASS   : tst_HelperRoundTrip::%s()" % name in out, "direct: %s did not pass:\n%s" % (name, out[-3000:]))
    _rule(box, "YES")
    out = _roundtrip(box, "pkexec", user="tuser")
    for name in ("whitespaceVariantsAreTwoDigestsEchoedExactly", "digestMismatchKeepsDirty", "inspectRoundTrip"):
        expect("PASS   : tst_HelperRoundTrip::%s()" % name in out, "pkexec: %s did not pass:\n%s" % (name, out[-3000:]))
    audit = box.run("journalctl --no-pager -o cat -t aether-serial-bridge").out
    expect("requested by uid 1000" in audit, "no audit line naming uid 1000 in the journal: %r" % audit[-1500:])
    _rule(box, "NO")
    out = _roundtrip(box, "pkexec-denied", user="tuser")
    expect("PASS   : tst_HelperRoundTrip::deniedByPolicyChangesNothing()" in out, "denied:\n%s" % out[-3000:])
    expect(box.active() == "active", "the service is not running at the end")
