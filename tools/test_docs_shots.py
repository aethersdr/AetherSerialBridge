#!/usr/bin/env python3
"""Tests for tools/docs_shots.py (INV-135, v1.1 WS-19): the generator refuses any fixture but the demo one, isolates
the settings, and refuses an image without its manifest. Standard library only (static-checks runs it): the test
build's hooks and the frame converter are small stand-in scripts.

Names the WS-19 document uses: refusesNonDemoConfig = test_refuses_non_demo_config, manifestPerImage =
test_manifest_written_per_image.
"""
import json
import os
import shutil
import stat
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import docs_shots  # noqa: E402
from test_check_docs_images import png  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PNG = png()

# A stand-in for tst_BridgeWindow / tst_Tui: records its environment, then writes what $FAKE_MODE says.
HOOK = r'''#!{python}
import json, os, sys
from pathlib import Path
raw = Path(os.environ["ASB_DOCS_SHOTS_DIR"])
name = Path(sys.argv[0]).name
log = Path(os.environ["FAKE_LOG"])
with log.open("a") as f:
    f.write(json.dumps({{"exe": name, "argv": sys.argv[1:], "env": dict(os.environ)}}) + "\n")
mode = os.environ.get("FAKE_MODE", "good")
png = bytes.fromhex("{png}")
def shot(stem, ext, manifest=True, text="Aether Serial Bridge"):
    (raw / (stem + ext)).write_bytes(png if ext == ".png" else b"\x1b[0mframe")
    if manifest:
        m = {{"image": stem + ".png", "texts": [text]}}
        if mode == "private-state":
            m["state"] = "connected from 10.0.0.5"
        (raw / (stem + ".json")).write_text(json.dumps(m))
if name.startswith("tst_BridgeWindow"):
    if os.environ.get("ASB_DOCS_SHOTS_HERO"):
        shot("window-running-dark@2x", ".png")
    else:
        shot("window-running-dark", ".png", manifest=(mode != "no-manifest"),
             text=("Client 10.0.0.5" if mode == "private" else "Amp"))
else:
    shot("tui-ports", ".ans")
'''

CONVERTER = r'''#!{python}
import sys
from pathlib import Path
if sys.argv[1] == "--optimize":
    sys.exit(0)
Path(sys.argv[2]).write_bytes(bytes.fromhex("{png}"))
'''


@unittest.skipIf(os.name == "nt", "the stand-in hooks are POSIX scripts")
class DocsShots(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = Path(self.tmp.name)
        self.bin = t / "bin"
        self.bin.mkdir()
        for n in ("tst_BridgeWindow", "tst_Tui"):
            p = self.bin / n
            p.write_text(HOOK.format(python=sys.executable, png=PNG.hex()))
            p.chmod(p.stat().st_mode | stat.S_IXUSR)
        self.converter = t / "conv.py"
        self.converter.write_text(CONVERTER.format(python=sys.executable, png=PNG.hex()))
        self.out = t / "out"
        self.log = t / "log.jsonl"
        self.base = dict(os.environ, FAKE_LOG=str(self.log))

    def tearDown(self):
        self.tmp.cleanup()

    def gen(self, fixture=None, mode="good", os_name="linux"):
        env = dict(self.base, FAKE_MODE=mode)
        return docs_shots.generate(os_name, self.bin, self.out, fixture or docs_shots.DEMO, str(self.converter),
                                   base_env=env)

    def calls(self):
        return [json.loads(l) for l in self.log.read_text().splitlines()] if self.log.exists() else []

    # refusesNonDemoConfig (the INV-135 mutant inv135-generator-accepts-any-config): a copy of the fixture, another
    # folder, a missing one: refused before any hook runs.
    def test_refuses_non_demo_config(self):
        copy = Path(self.tmp.name) / "my-station"
        shutil.copytree(docs_shots.DEMO, copy)
        for fixture in (copy, ROOT / "docs" / "images", Path(self.tmp.name) / "missing", ROOT / "tests" / "data"):
            with self.subTest(fixture=str(fixture)):
                with self.assertRaises(docs_shots.Refused):
                    self.gen(fixture)
                self.assertEqual(self.calls(), [])
        self.assertEqual(docs_shots.main(["--os", "linux", "--bin", str(self.bin), "--out", str(self.out),
                                          "--fixture", str(copy), "--converter", str(self.converter)]), 2)
        self.assertFalse(self.out.exists() and any(self.out.iterdir()))

    def test_demo_fixture_through_a_symlink_is_the_demo_fixture(self):
        link = Path(self.tmp.name) / "demo-link"
        link.symlink_to(docs_shots.DEMO, target_is_directory=True)
        self.gen(link)
        self.assertTrue((self.out / "window-running-dark.png").exists())

    # manifestPerImage: every image comes with its manifest; one without refuses the run and copies nothing.
    def test_manifest_written_per_image(self):
        self.gen()
        names = sorted(p.name for p in self.out.iterdir())
        self.assertEqual(names, ["tui-ports.json", "tui-ports.png", "window-running-dark.json",
                                 "window-running-dark.png", "window-running-dark@2x.json", "window-running-dark@2x.png"])
        shutil.rmtree(self.out)
        with self.assertRaisesRegex(RuntimeError, "window-running-dark.png: no manifest"):
            self.gen(mode="no-manifest")
        self.assertFalse(any(self.out.iterdir()))

    def test_manifest_with_a_finding_refuses(self):
        with self.assertRaisesRegex(RuntimeError, "address 10.0.0.5"):
            self.gen(mode="private")

    def test_a_finding_outside_the_texts_refuses(self):
        with self.assertRaisesRegex(RuntimeError, "address 10.0.0.5"):
            self.gen(mode="private-state")

    def test_settings_are_isolated(self):
        self.gen()
        calls = self.calls()
        self.assertEqual([c["exe"] for c in calls], ["tst_BridgeWindow", "tst_BridgeWindow", "tst_Tui"])
        real_home = os.path.expanduser("~")
        for c in calls:
            env = c["env"]
            home = env["ASB_DOCS_SHOTS_HOME"]
            self.assertNotEqual(os.path.realpath(home), os.path.realpath(real_home))
            for k in ("HOME", "USERPROFILE", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "APPDATA",
                      "LOCALAPPDATA"):
                self.assertTrue(env[k].startswith(home), f"{k}={env[k]}")
            self.assertEqual(env["QT_QPA_PLATFORM"], "offscreen")
            self.assertEqual(os.path.realpath(env["ASB_DOCS_FIXTURE"]), os.path.realpath(docs_shots.DEMO))
            self.assertNotIn("XDG_CONFIG_DIRS", env)
        self.assertEqual([c["env"].get("QT_SCALE_FACTOR") for c in calls[:2]], ["1", "2"])
        self.assertEqual(calls[1]["env"].get("ASB_DOCS_SHOTS_HERO"), "1")
        self.assertNotIn("ASB_DOCS_SHOTS_HERO", calls[0]["env"])
        self.assertFalse(Path(calls[0]["env"]["HOME"]).exists(), "the temporary home is removed")

    def test_windows_renders_on_the_native_platform(self):
        # Offscreen on Windows has no system font fallback: Inter lacks U+2715 (the close button) and U+25B4/U+25BE
        # (Details), which then render as boxes (docs-shots run 39103203793). Linux and the Pi stay offscreen, where
        # fontconfig supplies the fallback. Kills: the Windows job back on offscreen.
        for os_name, platform in (("windows", "windows"), ("linux", "offscreen"), ("pi", "offscreen")):
            with self.subTest(os=os_name), tempfile.TemporaryDirectory() as d:
                env = docs_shots.isolated_env(Path(d) / "home", Path(d) / "raw", docs_shots.DEMO, os_name, base={})
                self.assertEqual(env["QT_QPA_PLATFORM"], platform)

    def test_pi_runs_only_the_tui(self):
        self.gen(os_name="pi")
        self.assertEqual([c["exe"] for c in self.calls()], ["tst_Tui"])

    def test_refuses_a_non_empty_output(self):
        self.out.mkdir()
        (self.out / "old.png").write_bytes(PNG)
        with self.assertRaises(docs_shots.Refused):
            self.gen()


class TextEncoding(unittest.TestCase):
    """The hooks write their manifests and frames as UTF-8; on Windows Python's default text encoding is the ANSI code
    page (cp1252), which cannot decode them (docs-shots run 39102836031). Every text read or write in the docs tools
    must name its encoding. Kills: dropping encoding="utf-8" from docs_shots.py's manifest read."""

    TOOLS = ["docs_shots.py", "check_docs_images.py", "check_doc_links.py", "ansi_to_png.py"]

    @staticmethod
    def unencoded(source):
        import ast
        bad = []
        for node in ast.walk(ast.parse(source)):
            if not isinstance(node, ast.Call):
                continue
            f = node.func
            name = f.attr if isinstance(f, ast.Attribute) else f.id if isinstance(f, ast.Name) else None
            kws = {k.arg for k in node.keywords}
            if name in ("read_text", "write_text") and "encoding" not in kws:
                bad.append((name, node.lineno))
            elif name == "open" and not (isinstance(f, ast.Attribute) and isinstance(f.value, ast.Name)
                                         and f.value.id in ("tarfile", "Image")):
                mode = node.args[1] if len(node.args) > 1 else next(
                    (k.value for k in node.keywords if k.arg == "mode"), None)
                binary = isinstance(mode, ast.Constant) and "b" in str(mode.value)
                if not binary and "encoding" not in kws:
                    bad.append((name, node.lineno))
        return bad

    def test_the_checker_sees_a_missing_encoding(self):
        self.assertEqual(self.unencoded("p.read_text()\nopen(x)\n"), [("read_text", 1), ("open", 2)])
        self.assertEqual(self.unencoded("p.read_text(encoding='utf-8')\nopen(x, 'rb')\n"), [])

    def test_every_text_read_and_write_names_its_encoding(self):
        here = Path(__file__).resolve().parent
        for tool in self.TOOLS:
            with self.subTest(tool=tool):
                self.assertEqual(self.unencoded((here / tool).read_text(encoding="utf-8")), [])


if __name__ == "__main__":
    unittest.main()
