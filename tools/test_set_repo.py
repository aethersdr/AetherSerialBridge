#!/usr/bin/env python3
"""Tests for tools/set_repo.py: the rewrite moves every user-facing link and the CMake default, a second run changes
nothing, and --check fails on this project named at another repository (docs) or written out (code)."""
import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import set_repo  # noqa: E402

OLD = "old-owner/aether-serial-bridge"
NEW = "AetherSDR/AetherSerialBridge"


def tree(root):
    files = {
        "CMakeLists.txt": f'set(ASB_REPO "{OLD}" CACHE STRING\n    "x")\n',
        "README.md": f"From the [Releases page](https://github.com/{OLD}/releases).\n"
                     "[AetherSDR](https://github.com/aethersdr/AetherSDR); files in /etc/aether-serial-bridge.\n",
        "docs/linux.md": f"git clone https://github.com/{OLD}\n",
        "packaging/windows/README.txt": f"Documentation: https://github.com/{OLD}/blob/main/docs/windows.md\n",
        "src/x/Unit.cpp": '"Documentation=https://github.com/" ASB_REPO "\\n"\n',
    }
    for rel, text in files.items():
        p = Path(root) / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")


def run(*argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
        code = set_repo.main(list(argv))
    return code, out.getvalue()


class SetRepo(unittest.TestCase):
    def setUp(self):
        self._d = tempfile.TemporaryDirectory()
        self.root = Path(self._d.name)
        tree(self.root)

    def tearDown(self):
        self._d.cleanup()

    def read(self, rel):
        return (self.root / rel).read_text(encoding="utf-8")

    def test_clean_tree_passes(self):
        self.assertEqual(run("--root", str(self.root), "--check")[0], 0)

    def test_rewrite_moves_user_facing_files_and_cmake_only(self):
        code, out = run("--root", str(self.root), NEW)
        self.assertEqual(code, 0, out)
        for rel in ("CMakeLists.txt", "README.md", "docs/linux.md", "packaging/windows/README.txt"):
            self.assertIn(NEW, self.read(rel), rel)
            self.assertNotIn(OLD, self.read(rel), rel)
        self.assertIn("github.com/aethersdr/AetherSDR", self.read("README.md"))
        self.assertIn("/etc/aether-serial-bridge", self.read("README.md"))
        self.assertEqual(run("--root", str(self.root), "--check")[0], 0)

    def test_second_run_changes_nothing(self):
        run("--root", str(self.root), NEW)
        before = {p: p.read_bytes() for p in self.root.rglob("*") if p.is_file()}
        code, out = run("--root", str(self.root), NEW)
        self.assertEqual(code, 0)
        self.assertIn("0 file(s) changed", out)
        self.assertEqual(before, {p: p.read_bytes() for p in self.root.rglob("*") if p.is_file()})

    def test_check_fails_on_a_doc_naming_another_repository(self):
        p = self.root / "docs" / "linux.md"
        p.write_text(p.read_text() + "gh release download -R someone/aether-serial-bridge\n", encoding="utf-8")
        code, out = run("--root", str(self.root), "--check")
        self.assertEqual(code, 1)
        self.assertIn("docs/linux.md:2: someone/aether-serial-bridge is not ASB_REPO", out)

    def test_check_fails_on_a_link_written_out_in_code(self):
        (self.root / "src" / "x" / "About.cpp").write_text(f'"https://github.com/{OLD}"\n', encoding="utf-8")
        code, out = run("--root", str(self.root), "--check")
        self.assertEqual(code, 1)
        self.assertIn("src/x/About.cpp:1:", out)

    def test_check_ignores_vendored_files(self):
        p = self.root / "vendor" / "x" / "README.md"
        p.parent.mkdir(parents=True)
        p.write_text("https://github.com/x/aether-serial-bridge\n", encoding="utf-8")
        self.assertEqual(run("--root", str(self.root), "--check")[0], 0)

    def test_refuses_a_value_that_is_not_owner_name(self):
        self.assertEqual(run("--root", str(self.root), "not a repo\nExecStart=/bin/sh")[0], 2)
        self.assertIn(OLD, self.read("CMakeLists.txt"))


if __name__ == "__main__":
    unittest.main()
