#!/usr/bin/env python3
"""Negative tests for tools/check_win_test_names.py: each drift it exists to catch, planted in a copy of the tree.

Each case copies what the checker reads (itself, ci/, tests/win/, ci.yml, the station script, the product files
with the names) into a scratch tree, applies one mutation and runs the checker there.
"""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
COPIED = ("tools/check_win_test_names.py", "ci", "tests/win", ".github/workflows/ci.yml", "qualification",
          "src/core/ServicePlan.h", "src/core/FirewallPlan.h", "src/core/AppLocations.cpp",
          "src/platform/win/ProcessGuard.cpp", "src/platform/win/UninstallEntry.h")


def run_checker(mutations=()):
    with tempfile.TemporaryDirectory() as d:
        t = Path(d)
        for rel in COPIED:
            s, dst = ROOT / rel, t / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            if s.is_dir():
                shutil.copytree(s, dst)
            else:
                shutil.copy(s, dst)
        for rel, old, new in mutations:
            p = t / rel
            text = p.read_text(encoding="utf-8") if p.exists() else ""
            if old is not None:
                if old not in text:
                    raise AssertionError(f"fixture text not found in {rel}: {old!r}")
                text = text.replace(old, new, 1)
            else:
                text += new
            p.write_text(text, encoding="utf-8")
        r = subprocess.run([sys.executable, str(t / "tools" / "check_win_test_names.py"), "--root", str(t)],
                           capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr


class CheckWinTestNames(unittest.TestCase):
    def assertRejected(self, mutations, why):
        rc, out = run_checker(mutations)
        self.assertNotEqual(rc, 0, out)
        self.assertIn(why, out)

    def test_real_tree_passes(self):
        rc, out = run_checker()
        self.assertEqual(rc, 0, out)

    def test_scratch_prefix_written_out_in_a_test(self):
        self.assertRejected([("tests/win/integration/tst_WinApplierNative.cpp", None,
                              '\nstatic const char* k = "asb-ws6-x";\n')], "the ScratchPrefix name 'asb-ws6-'")

    def test_account_prefix_written_out_in_ci(self):
        self.assertRejected([(".github/workflows/ci.yml", None, "\n      - run: net user asbtest-0 /delete\n")],
                            "the AccountPrefix name")

    def test_fat_label_written_out_in_ci(self):
        self.assertRejected([(".github/workflows/ci.yml", "label=$($names.FatLabel)", "label=ASBFAT")],
                            "the FatLabel name 'ASBFAT'")

    def test_a_name_in_a_comment_is_fine(self):
        rc, out = run_checker([("tests/win/integration/tst_WinApplierNative.cpp", None,
                                "\n// scratch folders are asb-ws6-<tag>-<hex>\n")])
        self.assertEqual(rc, 0, out)

    def test_ci_stops_reading_the_source(self):
        self.assertRejected([(".github/workflows/ci.yml", "Import-PowerShellDataFile ci/win-test-names.psd1", "@{}")],
                            "ci.yml: does not read the names")

    def test_cmake_stops_generating_from_the_source(self):
        self.assertRejected([("tests/win/integration/CMakeLists.txt", '"${CMAKE_SOURCE_DIR}/ci/win-test-names.psd1"',
                              '"${CMAKE_SOURCE_DIR}/ci/other.psd1"')], "CMakeLists.txt: does not read the names")

    def test_product_group_drifts(self):
        self.assertRejected([("src/core/FirewallPlan.h", 'kGroup[] = "Aether Serial Bridge"',
                              'kGroup[] = "Aether Serial Bridge 2"')], "FirewallGroup is 'Aether Serial Bridge 2'")

    def test_product_service_name_drifts(self):
        self.assertRejected([("src/core/ServicePlan.h", 'kServiceName[] = "AetherSerialBridge"',
                              'kServiceName[] = "AetherBridge"')], "ServiceName is 'AetherBridge'")

    def test_install_folder_drifts(self):
        self.assertRejected([("src/platform/win/ProcessGuard.cpp", 'return root + QStringLiteral("AetherSerialBridge")',
                              'return root + QStringLiteral("AetherBridge")')], "InstallFolderLeaf is 'AetherBridge'")

    def test_uninstall_key_drifts(self):
        self.assertRejected([("src/platform/win/UninstallEntry.h", 'Uninstall\\\\AetherSerialBridge"',
                              'Uninstall\\\\AetherBridge"')], "UninstallKeyLeaf is 'AetherBridge'")

    def test_station_aside_drifts(self):
        self.assertRejected([("qualification/ws7-station.ps1", '"$LockDir.ws7-aside-$stamp"', '"$LockDir.aside-$stamp"')],
                            "AsideInfix is '.aside-'")

    def test_station_evidence_drifts(self):
        self.assertRejected([("qualification/ws7-station.ps1", '"$env:SystemDrive\\asb-ws7-station"',
                              '"$env:SystemDrive\\asb-station"')], "StationEvidenceLeaf is 'asb-station'")

    def test_source_loses_a_name(self):
        self.assertRejected([("ci/win-test-names.psd1", "    ReportLog ", "    # ReportLog ")],
                            "no single-quoted value for ReportLog")

    def test_unknown_name_in_the_source(self):
        self.assertRejected([("ci/win-test-names.psd1", "@{\n", "@{\n    NewThing = 'x'\n")],
                            "NewThing is not a known name")


if __name__ == "__main__":
    unittest.main()
