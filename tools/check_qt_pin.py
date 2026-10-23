#!/usr/bin/env python3
"""Static check: the Qt/aqt pin and the workflow rules (ci.yml, qt-static.yml, release.yml, docs-shots.yml).

cmake/qt-pin.env is the one place the Qt pin lives. Workflows cannot source it before their own
env block is evaluated, so .github/workflows/ci.yml spells the values out; this script fails the
static-checks leg when they disagree, and enforces the workflow rules that must not drift:

  * runner labels: GitHub-hosted only (HOSTED). The repository is public, so a fork pull request runs its own copy
    of a workflow: a self-hosted label anywhere would hand that machine to it
  * the arm64 switch: every job on an arm64 runner (a label ending in -arm) runs only while the repository variable
    ASB_ARM64_RUNNERS is not 'false' (ARM64_SWITCH in its `if:`), so one variable turns every arm64 job off
  * top-level `permissions: contents: read`
  * push builds main only (a pull request builds once, on pull_request), pull_request is not
    filtered to one branch, workflow_dispatch exists (a branch with no PR builds on demand)
  * every `uses:` is pinned to a full commit SHA (a local ./.github/workflows/ reusable workflow excepted), with
    no `# vN` comment: Dependabot bumps the SHA but not such a comment, so it drifts (AetherSDR's convention,
    .github/dependabot.yml)
  * the Qt package-revision check (ci/scripts/check_qt_revision.py) is wired into both installs

The runner, permission and pin rules apply to every product workflow: ci.yml, qt-static.yml, release.yml and
docs-shots.yml. Their own trigger rules: docs-shots.yml runs on workflow_dispatch only; qt-static.yml on
workflow_call (release.yml runs it first), workflow_dispatch and its weekly schedule, never on a push or a pull
request; release.yml on a push of a `v*` tag and on workflow_dispatch (the dry run), never on a pull request or a
branch push, and it calls qt-static.yml. Only release.yml's publish job may hold a write permission (contents:
write), and only under an `if:` that requires a tag push. The static Qt sources (packaging/qt-static/qt-static.env)
must name the pinned Qt version.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HOSTED = {
    "ubuntu-latest", "ubuntu-22.04", "ubuntu-24.04", "ubuntu-24.04-arm", "ubuntu-22.04-arm",
    "windows-latest", "windows-2022", "windows-2025",
}
# The arm64 switch: the exact text every arm64 job's `if:` carries.
ARM64_SWITCH = "vars.ASB_ARM64_RUNNERS != 'false'"
errors = []


def err(msg):
    errors.append(msg)


def read_env(path):
    out = {}
    for line in path.read_text().splitlines():
        m = re.match(r"^([A-Z0-9_]+)=(.*)$", line)
        if m:
            out[m.group(1)] = m.group(2).strip().strip('"')
    return out


pin = read_env(ROOT / "cmake" / "qt-pin.env")
for key in ("QT_VERSION", "QT_PACKAGE_REVISION", "QT_GUI_FLOOR", "QT_HEADLESS_FLOOR",
            "AQTINSTALL_VERSION", "AQTINSTALL_GIT_REF", "PY7ZR_VERSION"):
    if not pin.get(key):
        err(f"cmake/qt-pin.env: {key} missing")
if pin.get("QT_GUI_FLOOR") and pin.get("QT_VERSION"):
    if not pin["QT_VERSION"].startswith(pin["QT_GUI_FLOOR"]):
        err("QT_GUI_FLOOR is not a prefix of QT_VERSION")

ci_path = ROOT / ".github" / "workflows" / "ci.yml"
ci = ci_path.read_text()

# Workflow-level env: block (indented two spaces under a top-level `env:`).
m = re.search(r"^env:\n((?:[ ]{2}.*\n|\n)+)", ci, re.M)
env = {}
if m:
    for line in m.group(1).splitlines():
        km = re.match(r"^[ ]{2}([A-Z0-9_]+):\s*'?\"?([^'\"#\s]+)'?\"?", line)
        if km:
            env[km.group(1)] = km.group(2)
else:
    err("ci.yml: no top-level env: block")
for key in ("QT_VERSION", "QT_PACKAGE_REVISION", "AQTINSTALL_VERSION", "AQTINSTALL_GIT_REF",
            "PY7ZR_VERSION"):
    if env.get(key) != pin.get(key):
        err(f"ci.yml env {key}={env.get(key)!r} disagrees with cmake/qt-pin.env {pin.get(key)!r}")

for dockerfile in (ROOT / "ci" / "containers").glob("*.Dockerfile"):
    text = dockerfile.read_text()
    for key in ("QT_VERSION", "QT_PACKAGE_REVISION", "AQTINSTALL_VERSION"):
        dm = re.search(rf"^ENV {key}=(\S+)", text, re.M)
        if dm and dm.group(1) != pin.get(key):
            err(f"{dockerfile.name}: {key}={dm.group(1)} disagrees with cmake/qt-pin.env")

# The package-revision check must stay wired in. aqt cannot be told a revision, so what makes
# QT_PACKAGE_REVISION mean anything is ci/scripts/check_qt_revision.py: run after every install and
# on every cache hit, on both platforms, with the revision in every Qt cache key.
install_qt = (ROOT / "ci" / "scripts" / "install-qt.sh").read_text()
for sub in ("install", "evidence"):
    if f"check_qt_revision.py {sub}" not in ci:
        err(f"ci.yml (Windows): `check_qt_revision.py {sub}` is not run")
    if not re.search(rf"check_revision {sub}\b", install_qt):
        err(f"install-qt.sh (Linux): `check_revision {sub}` is not run")
if "check_qt_revision.py" not in install_qt:
    err("install-qt.sh does not call check_qt_revision.py")
for km in re.finditer(r"^\s*key:\s*(qt-.*)$", ci, re.M):
    if "env.QT_PACKAGE_REVISION" not in km.group(1):
        err(f"ci.yml: Qt cache key lacks the package revision: {km.group(1)}")

def job_blocks(text):
    """{job name: its text} for the two-space-indented keys under `jobs:`."""
    jobs = re.search(r"^jobs:\n(.*)", text, re.M | re.S)
    if not jobs:
        return None
    out = {}
    for block in re.split(r"^  (?=[A-Za-z0-9_-]+:\s*$)", jobs.group(1), flags=re.M):
        job = block.split(":", 1)[0].strip()
        if job and not job.startswith("#"):
            out[job] = block
    return out


def check_jobs(wf, text):
    """Runner labels, the arm64 switch and job-level permissions of every job in one workflow file."""
    for lm in re.finditer(r"^\s*runs-on:\s*(.+?)\s*$", text, re.M):
        value = lm.group(1).strip("'\"")
        if "self-hosted" in value:
            err(f"{wf}: runs-on {value}: self-hosted runners are not allowed (a fork pull request runs its own copy"
                f" of a workflow)")
        elif value not in HOSTED:
            err(f"{wf}: runs-on '{value}' is not a GitHub-hosted label")

    blocks = job_blocks(text)
    if blocks is None:
        err(f"{wf}: no jobs: block")
        return
    for job, block in blocks.items():
        # A job's own `if:` is the four-space-indented one (a step's `if:` is deeper and does not count).
        cond = re.search(r"^    if:\s*(.+?)\s*$", block, re.M)
        cond_text = cond.group(1) if cond else ""
        perm = re.search(r"^    permissions:\n((?:      .*\n)+)", block, re.M)
        if perm:
            lines = [l.strip() for l in perm.group(1).splitlines() if l.strip()]
            if not (wf == "release.yml" and job == "publish" and lines == ["contents: write"]):
                err(f"{wf}: job '{job}' sets permissions {lines}; only release.yml's publish job may, with contents: write")
            elif "github.event_name == 'push' && startsWith(github.ref, 'refs/tags/v')" not in cond_text:
                err(f"{wf}: job '{job}' writes but its `if:` does not require a tag push")
        runs_on = re.search(r"^    runs-on:\s*(.+?)\s*$", block, re.M)
        if runs_on and runs_on.group(1).strip("'\"").endswith("-arm") and ARM64_SWITCH not in cond_text:
            err(f"{wf}: arm64 job '{job}' runs without the arm64 switch: its `if:` must include `{ARM64_SWITCH}`")


def check_common(wf, text):
    """The rules every product workflow shares: jobs, top-level contents: read, actions pinned by SHA."""
    check_jobs(wf, text)
    if not re.search(r"^permissions:\n\s+contents: read\s*$", text, re.M):
        err(f"{wf}: top-level `permissions: contents: read` missing")
    for um in re.finditer(r"^\s*-?\s*uses:\s*(\S+)(.*)$", text, re.M):
        ref = um.group(1)
        if ref.startswith("./.github/workflows/"):
            continue
        if not re.match(r"^[\w.-]+/[\w./-]+@[0-9a-f]{40}$", ref):
            err(f"{wf}: action not pinned to a commit SHA: {ref}")


def on_block(text):
    m = re.search(r"^on:\n((?:[ ]{2}.*\n|[ ]*#.*\n|\n)+)", text, re.M)
    return m.group(1) if m else ""


check_common("ci.yml", ci)

# Permissions, triggers.
if "workflow_dispatch" not in ci:
    err("ci.yml: workflow_dispatch trigger missing")
push = re.search(r"^  push:\n    branches: \[(.*)\]\s*$", ci, re.M)
if not push:
    err("ci.yml: push trigger missing or not a one-line branches list")
elif [b.strip().strip("'\"") for b in push.group(1).split(",")] != ["main"]:
    err("ci.yml: push must build main only (a PR builds on pull_request; a branch on workflow_dispatch)")
pr = re.search(r"^  pull_request:\n((?:    .*\n)*)", ci, re.M)
if not pr:
    err("ci.yml: pull_request trigger missing")
elif "branches" in pr.group(1):
    err("ci.yml: pull_request is filtered by branch (workstream PRs target main)")

# The release workflows (WS-13).
wfdir = ROOT / ".github" / "workflows"
for wf in ("qt-static.yml", "release.yml"):
    if not (wfdir / wf).exists():
        err(f"{wf} is missing")
qs = (wfdir / "qt-static.yml").read_text() if (wfdir / "qt-static.yml").exists() else ""
rel = (wfdir / "release.yml").read_text() if (wfdir / "release.yml").exists() else ""
if qs:
    check_common("qt-static.yml", qs)
    trig = on_block(qs)
    keys = re.findall(r"^  ([a-z_]+):", trig, re.M)
    if not (set(keys) <= {"workflow_call", "workflow_dispatch", "schedule"}
            and {"workflow_call", "workflow_dispatch"} <= set(keys)):
        err(f"qt-static.yml: triggers {keys}; it runs on workflow_call and workflow_dispatch (and its schedule) only")
if rel:
    check_common("release.yml", rel)
    trig = on_block(rel)
    keys = re.findall(r"^  ([a-z_]+):", trig, re.M)
    if sorted(keys) != ["push", "workflow_dispatch"]:
        err(f"release.yml: triggers {keys}; it runs on a tag push and workflow_dispatch only")
    pm = re.search(r"^  push:\n((?:    .*\n)+)", trig, re.M)
    if not pm or [l.strip() for l in pm.group(1).splitlines() if l.strip()] != ["tags: ['v*']"]:
        err("release.yml: push must be exactly `tags: ['v*']` (no branches, no other filter)")
    if "publish" not in (job_blocks(rel) or {}):
        err("release.yml: no publish job")
    if not re.search(r"^  qt-static:\n    uses: \./\.github/workflows/qt-static\.yml\s*$", rel, re.M):
        err("release.yml: no qt-static job calling ./.github/workflows/qt-static.yml (the static Qt the builds restore)")

# The docs images workflow (v1.1 WS-19): the same runner, permission and pin rules; workflow_dispatch only (a
# person starts it and reviews what it renders); its Qt pin agrees with cmake/qt-pin.env.
ds_path = wfdir / "docs-shots.yml"
if not ds_path.exists():
    err("docs-shots.yml is missing")
else:
    ds = ds_path.read_text()
    check_common("docs-shots.yml", ds)
    keys = re.findall(r"^  ([a-z_]+):", on_block(ds), re.M)
    if keys != ["workflow_dispatch"]:
        err(f"docs-shots.yml: triggers {keys}; it runs on workflow_dispatch only")
    dm = re.search(r"^env:\n((?:[ ]{2}.*\n|\n)+)", ds, re.M)
    ds_env = {}
    for line in (dm.group(1).splitlines() if dm else []):
        km = re.match(r"^[ ]{2}([A-Z0-9_]+):\s*'?\"?([^'\"#\s]+)'?\"?", line)
        if km:
            ds_env[km.group(1)] = km.group(2)
    for key in ("QT_VERSION", "QT_PACKAGE_REVISION", "AQTINSTALL_VERSION"):
        if ds_env.get(key) != pin.get(key):
            err(f"docs-shots.yml env {key}={ds_env.get(key)!r} disagrees with cmake/qt-pin.env {pin.get(key)!r}")

# The static Qt sources name the pinned version.
qse = ROOT / "packaging" / "qt-static" / "qt-static.env"
if not qse.exists():
    err("packaging/qt-static/qt-static.env is missing")
else:
    src = read_env(qse)
    for key in ("QTBASE_TAR", "QTSERIALPORT_TAR", "QTBASE_ZIP", "QTSERIALPORT_ZIP"):
        if f"-src-{pin.get('QT_VERSION')}." not in src.get(key, ""):
            err(f"qt-static.env: {key}={src.get(key)!r} is not the pinned Qt {pin.get('QT_VERSION')}")
        if not re.fullmatch(r"[0-9a-f]{64}", src.get(key + "_SHA256", "")):
            err(f"qt-static.env: {key}_SHA256 is not a SHA-256")
    if f"/{pin.get('QT_VERSION')}/" not in src.get("QT_STATIC_BASE_URL", ""):
        err("qt-static.env: QT_STATIC_BASE_URL does not name the pinned Qt version")

if errors:
    print("check_qt_pin: FAILED")
    for e in errors:
        print("  -", e)
    sys.exit(1)
print("check_qt_pin: ok")
