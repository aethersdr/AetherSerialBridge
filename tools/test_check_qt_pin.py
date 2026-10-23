#!/usr/bin/env python3
"""Negative tests for tools/check_qt_pin.py: the checker runs on mutated copies of the workflows and must fail each.

Each case copies what the checker reads (itself, cmake/qt-pin.env, ci/scripts/install-qt.sh, the build
Dockerfiles, packaging/qt-static/qt-static.env, ci.yml, qt-static.yml, release.yml, docs-shots.yml) into a scratch
tree, applies one mutation to one of them and runs the checker there.
"""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WF = ROOT / ".github" / "workflows"
CI = WF / "ci.yml"
DS = WF / "docs-shots.yml"
ARM64_SWITCH = "vars.ASB_ARM64_RUNNERS != 'false'"


def run_checker(ci_text, others=None):
    """`others`: {file name under .github/workflows or a repo path: replacement text}."""
    others = others or {}
    with tempfile.TemporaryDirectory() as d:
        t = Path(d)
        for rel in ("tools/check_qt_pin.py", "cmake/qt-pin.env", "ci/scripts/install-qt.sh",
                    "packaging/qt-static/qt-static.env"):
            (t / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(ROOT / rel, t / rel)
        (t / "ci" / "containers").mkdir(parents=True, exist_ok=True)
        for f in (ROOT / "ci" / "containers").glob("*.Dockerfile"):
            shutil.copy(f, t / "ci" / "containers" / f.name)
        (t / ".github" / "workflows").mkdir(parents=True)
        (t / ".github" / "workflows" / "ci.yml").write_text(ci_text)
        for wf in ("qt-static.yml", "release.yml", "docs-shots.yml"):
            (t / ".github" / "workflows" / wf).write_text((WF / wf).read_text())
        for name, text in others.items():
            dest = t / ".github" / "workflows" / name if name.endswith(".yml") else t / name
            dest.write_text(text)
        r = subprocess.run([sys.executable, str(t / "tools" / "check_qt_pin.py")], capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr


def mutate(old, new, count=1):
    text = CI.read_text()
    if text.count(old) < 1:
        raise AssertionError(f"fixture text not found in ci.yml: {old!r}")
    return text.replace(old, new, count)


class CheckQtPin(unittest.TestCase):
    def assertRejected(self, ci_text, why):
        rc, out = run_checker(ci_text)
        self.assertNotEqual(rc, 0, out)
        self.assertIn(why, out)

    def assertOtherRejected(self, name, old, new, why):
        path = WF / name if name.endswith(".yml") else ROOT / name
        text = path.read_text()
        self.assertIn(old, text)
        rc, out = run_checker(CI.read_text(), {name: text.replace(old, new, 1)})
        self.assertNotEqual(rc, 0, out)
        self.assertIn(why, out)

    def test_real_workflow_passes(self):
        rc, out = run_checker(CI.read_text())
        self.assertEqual(rc, 0, out)

    # --- runners: GitHub-hosted only ---
    def test_self_hosted_label_set_is_rejected(self):
        self.assertRejected(mutate("runs-on: ubuntu-22.04", "runs-on: [self-hosted, Linux, X64]"),
                            "self-hosted runners are not allowed")

    def test_bare_self_hosted_label_is_rejected(self):
        self.assertRejected(mutate("runs-on: windows-latest", "runs-on: self-hosted"),
                            "self-hosted runners are not allowed")

    def test_block_list_runs_on_is_rejected(self):
        self.assertRejected(mutate("runs-on: ubuntu-22.04", "runs-on:\n      - self-hosted\n      - linux"),
                            "self-hosted runners are not allowed")

    def test_unknown_runner_label_is_rejected(self):
        self.assertRejected(mutate("runs-on: ubuntu-latest", "runs-on: build-box"),
                            "runs-on 'build-box' is not a GitHub-hosted label")

    def test_release_self_hosted_job_is_rejected(self):
        self.assertOtherRejected("release.yml", "    runs-on: windows-latest\n", "    runs-on: [self-hosted, Windows]\n",
                                 "release.yml: runs-on [self-hosted, Windows]: self-hosted runners are not allowed")

    # --- the arm64 switch ---
    def test_arm64_job_without_the_switch_is_rejected(self):
        self.assertRejected(mutate("    if: " + ARM64_SWITCH + "\n", ""),
                            "arm64 job 'distro-arm' runs without the arm64 switch")

    def test_arm64_switch_on_a_step_does_not_count(self):
        # Kills: the switch found anywhere in the job (a step's `if:` is deeper than the job's own).
        text = mutate("    if: " + ARM64_SWITCH + "\n", "")
        before, after = text.split("\n  distro-arm:\n", 1)
        after = after.replace("    steps:\n", "    steps:\n      - if: " + ARM64_SWITCH + "\n        run: true\n", 1)
        self.assertRejected(before + "\n  distro-arm:\n" + after, "arm64 job 'distro-arm' runs without the arm64 switch")

    def test_release_arm64_job_without_the_switch_is_rejected(self):
        self.assertOtherRejected("release.yml", " && " + ARM64_SWITCH + " }}\n", " }}\n",
                                 "release.yml: arm64 job 'linux-arm64' runs without the arm64 switch")

    def test_qt_static_arm64_job_without_the_switch_is_rejected(self):
        self.assertOtherRejected("qt-static.yml", "    if: " + ARM64_SWITCH + "\n", "",
                                 "qt-static.yml: arm64 job 'linux-arm64' runs without the arm64 switch")

    def test_docs_shots_arm64_job_without_the_switch_is_rejected(self):
        self.assertOtherRejected("docs-shots.yml", "    if: " + ARM64_SWITCH + "\n", "",
                                 "docs-shots.yml: arm64 job 'pi' runs without the arm64 switch")

    # --- ci.yml triggers ---
    def test_push_builds_workstream_branches(self):
        self.assertRejected(mutate("    branches: [main]\n", "    branches: [main, 'ws-*']\n"),
                            "push must build main only")

    def test_pull_request_filtered_to_a_branch_is_rejected(self):
        self.assertRejected(mutate("  pull_request:\n", "  pull_request:\n    branches: [main]\n"),
                            "pull_request is filtered by branch")

    def test_without_workflow_dispatch_is_rejected(self):
        self.assertRejected(mutate("  workflow_dispatch:\n", ""), "workflow_dispatch trigger missing")

    # --- actions pinned by SHA ---
    def test_short_sha_is_rejected(self):
        self.assertRejected(mutate("actions/checkout@11d5960a326750d5838078e36cf38b85af677262", "actions/checkout@11d5960"),
                            "action not pinned to a commit SHA: actions/checkout@11d5960")

    def test_unpinned_action_is_rejected(self):
        self.assertRejected(mutate("actions/checkout@11d5960a326750d5838078e36cf38b85af677262", "actions/checkout@v4"),
                            "action not pinned to a commit SHA: actions/checkout@v4")

    def test_remote_reusable_workflow_at_a_tag_is_rejected(self):
        # Kills: the local-workflow exemption matching any reusable workflow.
        self.assertOtherRejected("release.yml", "    uses: ./.github/workflows/qt-static.yml\n",
                                 "    uses: someone/else/.github/workflows/qt-static.yml@v1\n",
                                 "action not pinned to a commit SHA: someone/else/.github/workflows/qt-static.yml@v1")

    # --- the Qt pin ---
    def test_ci_qt_pin_disagreeing_is_rejected(self):
        self.assertRejected(mutate("  QT_VERSION: '6.12.0'", "  QT_VERSION: '6.11.0'"),
                            "ci.yml env QT_VERSION='6.11.0' disagrees")

    def test_qt_cache_key_without_the_revision_is_rejected(self):
        self.assertRejected(mutate("key: qt-${{ env.QT_VERSION }}-${{ env.QT_PACKAGE_REVISION }}-linux",
                                   "key: qt-${{ env.QT_VERSION }}-linux"),
                            "Qt cache key lacks the package revision")

    def test_revision_evidence_check_dropped_is_rejected(self):
        self.assertRejected(mutate("check_qt_revision.py evidence", "check_qt_revision.py evidense", count=10),
                            "`check_qt_revision.py evidence` is not run")

    # --- the release workflows (WS-13) ---
    def test_release_on_pull_request_is_rejected(self):
        self.assertOtherRejected("release.yml", "  workflow_dispatch:\n", "  pull_request:\n  workflow_dispatch:\n",
                                 "it runs on a tag push and workflow_dispatch only")

    def test_release_on_a_branch_push_is_rejected(self):
        self.assertOtherRejected("release.yml", "    tags: ['v*']\n", "    tags: ['v*']\n    branches: [main]\n",
                                 "push must be exactly")

    def test_publish_without_tag_condition_is_rejected(self):
        self.assertOtherRejected("release.yml", "if: github.event_name == 'push' && startsWith(github.ref, 'refs/tags/v')",
                                 "if: github.event_name == 'workflow_dispatch'", "does not require a tag push")

    def test_write_permission_outside_publish_is_rejected(self):
        self.assertOtherRejected("release.yml", "    timeout-minutes: 20\n",
                                 "    permissions:\n      contents: write\n    timeout-minutes: 20\n",
                                 "only release.yml's publish job may")

    def test_release_without_the_qt_static_job_is_rejected(self):
        self.assertOtherRejected("release.yml", "    uses: ./.github/workflows/qt-static.yml\n",
                                 "    uses: ./.github/workflows/other.yml\n", "release.yml: no qt-static job")

    def test_qt_static_on_push_is_rejected(self):
        self.assertOtherRejected("qt-static.yml", "on:\n", "on:\n  push:\n    branches: [main]\n",
                                 "it runs on workflow_call and workflow_dispatch")

    def test_qt_static_without_workflow_call_is_rejected(self):
        # release.yml runs it as its first job.
        self.assertOtherRejected("qt-static.yml", "  workflow_call:\n", "", "it runs on workflow_call and workflow_dispatch")

    def test_qt_static_unpinned_action_is_rejected(self):
        self.assertOtherRejected("qt-static.yml", "actions/checkout@11d5960a326750d5838078e36cf38b85af677262",
                                 "actions/checkout@v4", "action not pinned to a commit SHA")

    def test_static_sources_for_another_qt_are_rejected(self):
        self.assertOtherRejected("packaging/qt-static/qt-static.env", "QTBASE_TAR=qtbase-everywhere-src-6.12.0.tar.xz",
                                 "QTBASE_TAR=qtbase-everywhere-src-6.11.0.tar.xz", "is not the pinned Qt")

    # --- v1.1 WS-19: docs-shots.yml is held to the same rules, and runs on workflow_dispatch only ---
    def ds_rejected(self, old, new, why):
        text = DS.read_text()
        self.assertIn(old, text)
        rc, out = run_checker(CI.read_text(), {"docs-shots.yml": text.replace(old, new, 1)})
        self.assertNotEqual(rc, 0, out)
        self.assertIn(why, out)

    def test_docs_shots_on_push_is_rejected(self):
        self.ds_rejected("on:\n  workflow_dispatch:\n", "on:\n  push:\n    branches: [ws-19]\n  workflow_dispatch:\n",
                         "docs-shots.yml: triggers ['push', 'workflow_dispatch']")

    def test_docs_shots_on_pull_request_is_rejected(self):
        self.ds_rejected("on:\n  workflow_dispatch:\n", "on:\n  pull_request:\n  workflow_dispatch:\n",
                         "docs-shots.yml: triggers ['pull_request', 'workflow_dispatch']")

    def test_docs_shots_write_permission_is_rejected(self):
        self.ds_rejected("permissions:\n  contents: read\n", "permissions:\n  contents: write\n",
                         "docs-shots.yml: top-level `permissions: contents: read` missing")

    def test_docs_shots_self_hosted_runner_is_rejected(self):
        self.ds_rejected("runs-on: ubuntu-24.04-arm", "runs-on: [self-hosted, Linux, ARM64]",
                         "docs-shots.yml: runs-on [self-hosted, Linux, ARM64]: self-hosted runners are not allowed")

    def test_docs_shots_unpinned_action_is_rejected(self):
        self.ds_rejected("actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02", "actions/upload-artifact@v4",
                         "docs-shots.yml: action not pinned to a commit SHA")

    def test_docs_shots_qt_pin_disagreeing_is_rejected(self):
        self.ds_rejected("  QT_VERSION: '6.12.0'", "  QT_VERSION: '6.11.0'", "docs-shots.yml env QT_VERSION='6.11.0' disagrees")


if __name__ == "__main__":
    unittest.main()
