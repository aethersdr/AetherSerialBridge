#!/usr/bin/env python3
"""Tests for tools/check_doc_links.py (v1.1 WS-19): a missing image, a missing page or a wrong #heading fails."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_doc_links as links  # noqa: E402


class CheckDocLinks(unittest.TestCase):
    def tree(self, readme, guide="# Guide\n\n## The window's buttons\n\n## The window (x64, or a Pi)\n"):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        root = Path(d.name)
        (root / "docs" / "images" / "linux").mkdir(parents=True)
        (root / "docs" / "images" / "linux" / "a.png").write_bytes(b"x")
        (root / "docs" / "guide.md").write_text(guide)
        (root / "README.md").write_text(readme)
        return links.check(root)

    def test_good_links_pass(self):
        self.assertEqual(self.tree("![a](docs/images/linux/a.png) [g](docs/guide.md#the-windows-buttons) "
                                   "[w](docs/guide.md#the-window-x64-or-a-pi) <img src=\"docs/images/linux/a.png\"> "
                                   "[x](https://example.com/y) `[not](a-link.md)`\n"), [])

    def test_missing_image_fails(self):
        f = self.tree("<img src=\"docs/images/windows/b.png\" alt=\"b\">\n![c](docs/images/linux/c.png)\n")
        self.assertEqual(len(f), 2, f)
        self.assertTrue(all("no such file" in x for x in f), f)

    def test_wrong_heading_fails(self):
        f = self.tree("[g](docs/guide.md#the-window)\n")
        self.assertEqual(f, ["README.md: docs/guide.md#the-window: no heading #the-window in docs/guide.md"])

    def test_links_in_code_fences_are_ignored(self):
        self.assertEqual(self.tree("```\n![a](missing.png)\n```\n"), [])


if __name__ == "__main__":
    unittest.main()
