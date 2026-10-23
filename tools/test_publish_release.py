#!/usr/bin/env python3
"""Tests for packaging/publish-release.py: the existing-release guard sees drafts and pages."""
import importlib.util
import os
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "packaging" / "publish-release.py"


def load():
    spec = importlib.util.spec_from_file_location("publish_release", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class PublishGuard(unittest.TestCase):
    def setUp(self):
        self.mod = load()
        self.dir = tempfile.mkdtemp()
        for name in ("SHA256SUMS", "aether-serial-bridge-linux-x64"):
            Path(self.dir, name).write_text("x")
        os.environ["GITHUB_TOKEN"] = "test-token"
        self.calls = []

    def fake(self, pages):
        def call(method, url, token, body=None, ctype="application/json"):
            self.calls.append((method, url))
            if method == "GET":
                page = int(url.rsplit("page=", 1)[1])
                return pages[page - 1] if page <= len(pages) else []
            if url.endswith("/releases"):
                return {"upload_url": "https://uploads.example/assets{?name}", "html_url": "draft"}
            return {}
        self.mod.call = call

    def posts(self):
        return [u for m, u in self.calls if m == "POST"]

    def test_existing_draft_is_refused(self):
        self.fake([[{"tag_name": "v0.1.0", "draft": True, "html_url": "x"}]])
        self.assertEqual(self.mod.main(["p", "o/r", "v0.1.0", self.dir]), 1)
        self.assertEqual(self.posts(), [])

    def test_existing_release_on_a_later_page_is_refused(self):
        page1 = [{"tag_name": "v9.%d" % i, "draft": False} for i in range(100)]
        self.fake([page1, [{"tag_name": "v0.1.0", "draft": False, "html_url": "x"}]])
        self.assertEqual(self.mod.main(["p", "o/r", "v0.1.0", self.dir]), 1)
        self.assertEqual(self.posts(), [])

    def test_new_tag_creates_one_draft_and_uploads(self):
        self.fake([[{"tag_name": "v0.0.9", "draft": False}]])
        self.assertEqual(self.mod.main(["p", "o/r", "v0.1.0", self.dir]), 0)
        posts = self.posts()
        self.assertTrue(posts[0].endswith("/repos/o/r/releases"))
        self.assertEqual(len(posts), 3)   # the draft, then the two files

    def test_missing_sums_is_refused(self):
        Path(self.dir, "SHA256SUMS").unlink()
        self.fake([[]])
        self.assertEqual(self.mod.main(["p", "o/r", "v0.1.0", self.dir]), 1)


if __name__ == "__main__":
    unittest.main()
