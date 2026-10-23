"""Build identity on Linux (v1.1 WS-14, D61, INV-120): the installed program's SHA-256 against this build's.

The installed program's own helper (what the window's day-to-day operations run through pkexec) hashes its own image,
which IS the installed file, so it classifies its own installation as Current and v1.0's route keeps working. A
different build of the headless program, run from a download, sees "a different build": --apply refuses and names
--install (as Windows since #21); --install replaces without asking (the CLI's rule)."""

from sysd import CONFIG_A, EXE, INSTALLED, case, expect, running_with

from cases.helper import _bytes, helper, request

DOWNLOAD = "/root/dl/aether-serial-bridge-linux-download"


def sha(box, path):
    return box.sh("sha256sum %s" % path).split()[0]


@case("installed_helper_is_current")
def installed_helper_is_current(box):
    """INV-120: the installed program's helper hashes its own image (the installed file): Current, so Save & apply,
    Stop and Start through it work with no question; --status from the installed program says installed."""
    r = box.cli("--install")
    expect(r.rc == 0, "--install: %r" % r)
    rc, res, out, err = helper(box, request("save-and-apply", _bytes(CONFIG_A)))
    expect(rc == 0 and res and res["outcome"] == "Ok", "save-and-apply through the installed helper: rc=%s out=%r err=%r"
           % (rc, out, err))
    running_with(box, CONFIG_A["ports"][0]["tcpPort"])
    rc, res, out, err = helper(box, request("stop"))
    expect(rc == 0 and res and res["outcome"] == "Ok", "stop: rc=%s out=%r err=%r" % (rc, out, err))
    box.wait_until(lambda: box.active() in ("inactive", "failed"), timeout=30, what="the stop")
    rc, res, out, err = helper(box, request("start"))
    expect(rc == 0 and res and res["outcome"] == "Ok", "start: rc=%s out=%r err=%r" % (rc, out, err))
    status = box.run("%s --status" % INSTALLED)
    expect("did not finish" not in status.out and "different build" not in status.out,
           "--status of the installed program: %r" % status)


@case("downloaded_other_build_apply_refused")
def downloaded_other_build_apply_refused(box):
    """INV-120: a different build of the headless program (here: the program with one byte appended, so its SHA-256
    differs and it still runs) sees the installation as another build: --apply refuses, names --install and changes
    nothing; --install from it replaces the installed program without asking."""
    r = box.cli("--install")
    expect(r.rc == 0, "--install: %r" % r)
    rc, res, out, err = helper(box, request("save-and-apply", _bytes(CONFIG_A)))
    expect(rc == 0 and res and res["outcome"] == "Ok", "save-and-apply: rc=%s out=%r err=%r" % (rc, out, err))
    running_with(box, CONFIG_A["ports"][0]["tcpPort"])
    box.sh("mkdir -p /root/dl && cp %s %s && printf x >> %s && chmod 0755 %s" % (EXE, DOWNLOAD, DOWNLOAD, DOWNLOAD))
    installed = sha(box, INSTALLED)
    expect(sha(box, DOWNLOAD) != installed, "the download is not a different build")
    pid = box.main_pid()
    r = box.run("%s --apply" % DOWNLOAD)
    msg = r.out + r.err
    expect(r.rc != 0 and "--install" in msg, "--apply from a different build was not refused: %r" % r)
    expect("different build" in msg or "incomplete" in msg, "the refusal does not say why: %r" % r)
    expect(sha(box, INSTALLED) == installed, "the refused --apply replaced the installed program")
    expect(box.active() == "active" and box.main_pid() == pid, "the refused --apply touched the running service")
    status = box.run("%s --status" % DOWNLOAD)
    expect("different build" in status.out, "--status from the download does not name another build: %r" % status)
    r = box.run("%s --install" % DOWNLOAD)
    expect(r.rc == 0, "--install from the download: %r" % r)
    expect(sha(box, INSTALLED) == sha(box, DOWNLOAD), "--install did not install the download's bytes")
    rc, res, out, err = helper(box, request("stop"))
    expect(rc == 0 and res and res["outcome"] == "Ok", "the new build's helper is Current: rc=%s out=%r err=%r"
           % (rc, out, err))
