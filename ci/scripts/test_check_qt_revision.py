#!/usr/bin/env python3
"""Tests for check_qt_revision.py: run by the static-checks leg (python3 -m unittest)."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "check_qt_revision.py"
BASE = "https://download.qt.io/online/qtsdkrepository/linux_x64/desktop/qt6_6120/qt6_6120"


def line(package, qt, revision, archive, build="0"):
    return (f"2026-10-13 11:30:54,230 - aqt.installer - DEBUG - installer 1 Download URL: "
            f"{BASE}/{package}/{qt}-{build}-{revision}{archive}")


GOOD = [
    line("qt.qt6.6120.addons.qtserialport.linux_gcc_64", "6.12.0", "202609280346",
         "qtserialport-Linux-RHEL_9_6-GCC-Linux-RHEL_9_6-X86_64.7z"),
    line("qt.qt6.6120.linux_gcc_64", "6.12.0", "202609280346",
         "qtbase-Linux-RHEL_9_6-GCC-Linux-RHEL_9_6-X86_64.7z"),
    line("qt.qt6.6120.linux_gcc_64", "6.12.0", "202609280346", "icu-linux-Rhel8.6-x86_64.7z"),
]
NOISE = "2026-10-13 11:30:48,230 - aqt.helper - DEBUG - helper 1 Attempt to download checksum at x"


class Check(unittest.TestCase):
    def run_script(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), *args], capture_output=True, text=True)

    def install(self, lines, revision="202609280346", version="6.12.0"):
        with tempfile.TemporaryDirectory() as d:
            log, out = Path(d, "aqtinstall.log"), Path(d, "ev", "stamp")
            log.write_text("\n".join(lines) + "\n")
            r = self.run_script("install", "--log", str(log), "--version", version,
                                "--revision", revision, "--out", str(out))
            return r, (out.read_text() if out.exists() else None)

    def test_matching_revision_passes_and_writes_evidence(self):
        r, ev = self.install([NOISE, *GOOD])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(ev.startswith("qt 6.12.0 package-revision 202609280346\n"))
        self.assertIn("qt.qt6.6120.linux_gcc_64/qtbase-Linux", ev)

    def test_a_wrong_pinned_revision_fails_and_writes_nothing(self):
        r, ev = self.install([NOISE, *GOOD], revision="202609280347")
        self.assertEqual(r.returncode, 1)
        self.assertIn("package revision 202609280346, pinned 202609280347", r.stderr)
        self.assertIsNone(ev)

    def test_one_archive_of_another_revision_fails(self):
        mixed = [*GOOD, line("qt.qt6.6120.linux_gcc_64", "6.12.0", "202610010101", "qtsvg-Linux.7z")]
        r, ev = self.install(mixed)
        self.assertEqual(r.returncode, 1)
        self.assertIn("qtsvg-Linux.7z: package revision 202610010101", r.stderr)
        self.assertIsNone(ev)

    def test_another_qt_version_fails(self):
        r, _ = self.install(GOOD, version="6.12.1")
        self.assertEqual(r.returncode, 1)
        self.assertIn("Qt 6.12.0, pinned 6.12.1", r.stderr)

    def test_a_log_with_no_downloads_fails(self):
        r, ev = self.install([NOISE])
        self.assertEqual(r.returncode, 1)
        self.assertIn("no 'Download URL:' lines", r.stderr)
        self.assertIsNone(ev)

    def test_an_unparseable_url_fails(self):
        r, _ = self.install([*GOOD, "x - DEBUG - Download URL: https://example.org/qtbase.7z"])
        self.assertEqual(r.returncode, 1)
        self.assertIn("unrecognised archive URL", r.stderr)

    def test_a_missing_required_archive_fails(self):
        r, _ = self.install([GOOD[1], GOOD[2]])
        self.assertEqual(r.returncode, 1)
        self.assertIn("no qtserialport-* archive", r.stderr)
        r, _ = self.install([GOOD[0], GOOD[2]])
        self.assertIn("no qtbase-* archive", r.stderr)

    def test_a_missing_log_fails(self):
        r = self.run_script("install", "--log", "/nonexistent/aqtinstall.log", "--version", "6.12.0",
                            "--revision", "1", "--out", "/nonexistent/out")
        self.assertEqual(r.returncode, 1)

    def test_evidence_roundtrip_and_rejections(self):
        with tempfile.TemporaryDirectory() as d:
            log, ev = Path(d, "log"), Path(d, "stamp")
            log.write_text("\n".join(GOOD) + "\n")
            args = ("--version", "6.12.0", "--revision", "202609280346")
            self.assertEqual(self.run_script("install", "--log", str(log), "--out", str(ev), *args).returncode, 0)
            self.assertEqual(self.run_script("evidence", "--file", str(ev), *args).returncode, 0)
            # Another revision: the proof is for a different pin.
            r = self.run_script("evidence", "--file", str(ev), "--version", "6.12.0", "--revision", "202609280347")
            self.assertEqual(r.returncode, 1)
            # The empty stamp the unchecked installer used to write.
            ev.write_text("")
            r = self.run_script("evidence", "--file", str(ev), *args)
            self.assertEqual(r.returncode, 1)
            self.assertIn("installed without a revision check", r.stderr)
            # Right header, no archives.
            ev.write_text("qt 6.12.0 package-revision 202609280346\n")
            self.assertEqual(self.run_script("evidence", "--file", str(ev), *args).returncode, 1)
            # No file at all.
            self.assertEqual(self.run_script("evidence", "--file", str(Path(d, "none")), *args).returncode, 1)


if __name__ == "__main__":
    unittest.main()
