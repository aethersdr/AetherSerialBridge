#!/usr/bin/env python3
"""Tests for tools/check_docs_images.py (INV-135, v1.1 WS-19), run in static-checks.

The controls plant one finding each in a copy of the repository's docs/images/demo and expect the gate to fail; the
clean rows expect it to pass. Standard library only (the static-checks job installs no Pillow): PNGs are built by hand.
"""
import json
import shutil
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_docs_images as gate  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def chunk(ctype, data):
    return struct.pack(">I", len(data)) + ctype + data + struct.pack(">I", zlib.crc32(ctype + data) & 0xFFFFFFFF)


def png(text_chunk=None):
    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
    body = chunk(b"IHDR", ihdr)
    if text_chunk:
        body += chunk(b"tEXt", text_chunk)
    body += chunk(b"IDAT", zlib.compress(b"\x00\x00\x00\x00"))
    return b"\x89PNG\r\n\x1a\n" + body + chunk(b"IEND", b"")


class Tree:
    """A repository root holding a copy of the real fixture and the given images."""

    def __init__(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        shutil.copytree(ROOT / "docs" / "images" / "demo", self.root / "docs" / "images" / "demo")

    def image(self, rel, texts=None, manifest=True, data=None):
        p = self.root / "docs" / "images" / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data if data is not None else png())
        if manifest:
            p.with_suffix(".json").write_text(json.dumps({"image": p.name, "texts": texts or ["Aether Serial Bridge"]}))
        return p

    def findings(self):
        return gate.check(self.root)

    def close(self):
        self.dir.cleanup()


class CheckDocsImages(unittest.TestCase):
    def setUp(self):
        self.t = Tree()

    def tearDown(self):
        self.t.close()

    def assertFinds(self, needle):
        f = self.t.findings()
        self.assertTrue(any(needle in x for x in f), f"no finding with {needle!r}: {f}")

    def test_repository_is_clean(self):
        self.assertEqual(gate.check(ROOT), [])

    def test_clean_manifest_passes(self):
        self.t.image("linux/window.png", ["Amp", "192.0.2.20:4001", "From 192.0.2.10:50514", "0.0.0.0", "127.0.0.1",
                                          "2001:db8::20", "/etc/aether-serial-bridge/config.json",
                                          "C:\\ProgramData\\AetherSerialBridge\\config.json", "sha256 0123456789ab",
                                          "Updated 12:00:05", "1.1.0", "COM5", "8N1", "FT232R",
                                          "Connected from 192.0.2.10.", "Version 1.2.3.4.5", "x86_64", "aarch64",
                                          "40 x 20", "80x24", "sha256", "utf8", "if00-port0", "DEMO0001", "2x"])
        self.assertEqual(self.t.findings(), [])

    # The control (the INV-135 mutant inv135-checker-skips-addresses): a planted private address fails the gate.
    def test_control_private_address_fails(self):
        for addr in ("10.0.0.5", "192.168.1.20:4001", "172.16.4.2", "100.64.1.9", "8.8.8.8", "fd00::5", "[fe80::1]"):
            with self.subTest(addr=addr):
                self.t.image("linux/window.png", ["Connected from " + addr])
                self.assertFinds("outside the documentation ranges")
        # An address that ends a sentence: footers and results are sentences, and several wrap the OS's error text.
        for text in ("Connected from 10.0.0.5.", "Could not bind 10.0.0.5: the address is not available.",
                     "Apply failed: could not bind 192.168.1.20.", "Listening on 172.16.4.2.\nSaved."):
            with self.subTest(text=text):
                self.t.image("linux/window.png", [text])
                self.assertFinds("outside the documentation ranges")

    # IPv6 is scanned as a whole token (all eight groups, leading or trailing "::", an IPv4 tail) before it is parsed,
    # in a manifest and in a fixture file alike; an address-like token that does not parse is reported too.
    V6_PRIVATE = (("fd00:1:2:3:4:5:6::", "fd00:1:2:3:4:5:6::"), ("fd00::1", "fd00::1"),
                  ("::ffff:10.0.0.5", "::ffff:10.0.0.5"), ("fd12:3456:789a:1:2:3:4:5", "fd12:3456:789a:1:2:3:4:5"),
                  ("Bound to fd00::1.", "fd00::1"), ("Connected from [fd00::5]:4001", "fd00::5"),
                  ("Listening on fe80::1%2", "fe80::1"), ("fd00:::1", "fd00:::1"))

    def test_control_private_ipv6_fails_in_a_manifest(self):
        for text, addr in self.V6_PRIVATE:
            with self.subTest(text=text):
                self.t.image("linux/window.png", [text])
                self.assertFinds(f"address {addr} ")

    def test_control_private_ipv6_fails_in_the_fixture(self):
        cfg = self.t.root / "docs" / "images" / "demo" / "environment-linux.json"
        original = cfg.read_text(encoding="utf-8")
        for text, addr in self.V6_PRIVATE:
            with self.subTest(text=text):
                data = json.loads(original)
                data["planted"] = text
                cfg.write_text(json.dumps(data), encoding="utf-8")
                self.assertFinds(f"environment-linux.json: address {addr} ")

    def test_documentation_ipv6_passes(self):
        self.t.image("linux/window.png", ["2001:db8::20", "2001:db8:0:0:0:0:0:1", "[2001:db8::5]:4001", "2001:db8::/32",
                                          "::1", "::", "Connected from 2001:db8::10.", "2001:DB8::A", "Updated 12:00:05",
                                          "::ffff:192.0.2.10", "fe80 is link-local"])
        self.assertEqual(self.t.findings(), [])

    # The control: a planted callsign-shaped word fails the gate.
    def test_control_callsign_fails(self):
        self.t.image("windows/window.png", ["Operator QQ0QQ"])
        self.assertFinds("callsign-shaped word 'QQ0QQ'")
        # Prefixes that start with a digit, and callsigns typed in lower case (a device label, a port name).
        for word in ("2E0QQQ", "9Q1QQQ", "3QQ0QQ", "q1qq", "Qq0Qq"):
            with self.subTest(word=word):
                self.t.image("windows/window.png", ["Port " + word])
                self.assertFinds(f"callsign-shaped word {word!r}")

    # The fixture's own words never trip the gate: every string in every fixture file, as one manifest.
    def test_fixture_words_do_not_false_positive(self):
        texts = []
        for f in sorted((ROOT / "docs" / "images" / "demo").glob("*.json")):
            if f.name != "allow-list.json":
                texts += list(gate.strings_in(json.loads(f.read_text())))
        self.assertTrue(len(texts) > 20)
        self.t.image("linux/fixture.png", texts)
        self.assertEqual(self.t.findings(), [])

    def test_fixture_is_held_to_the_rules(self):
        cfg = self.t.root / "docs" / "images" / "demo" / "config-linux.json"
        cfg.write_text(cfg.read_text().replace("192.0.2.10", "10.1.2.3"))
        self.assertFinds("config-linux.json: address 10.1.2.3")

    def test_unknown_paths_and_hosts_fail(self):
        for text, needle in (("/home/runner/work/asb/asb/tool", "path '/home/runner"),
                             ("C:\\actions-runner\\_work\\x", "path 'C:\\\\actions-runner"),
                             ("sudo \"/opt/build/aether.AppImage\" --install", "path '/opt/build"),
                             ("ssh station.internal", "host name 'station.internal'")):
            with self.subTest(text=text):
                self.t.image("linux/window.png", [text])
                self.assertFinds(needle)

    # Allowed paths match whole components, and a path the screen cut short ("…") keeps at least one whole one.
    def test_paths_match_whole_components(self):
        for text in ("/usr/local/binary-anything", "/home/demox/notes", "/h\u2026", "/\u2026", "C:\\P\u2026",
                     "/usr/local/binx\u2026", "/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_DEMO9999-if00-port0",
                     "C:\\Program Files\\AetherSerialBridgeX\\x.exe"):
            with self.subTest(text=text):
                self.t.image("linux/window.png", [text])
                self.assertFinds("is not on the allow-list")
        self.t.image("linux/window.png", ["/usr/local/bin/aether-serial-bridge", "/dev/serial/b\u2026",
                                          "/dev/ser\u2026", "/usr/local/bin/aether-ser\u2026", "/bin/sh",
                                          "/home/demo/Downloads/x.AppImage",
                                          "C:\\Program Files\\AetherSerialBridge\\aether-serial-bridge.exe",
                                          "/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_DEMO0002-if00-port0"])
        self.assertEqual(self.t.findings(), [])

    # Every field of a manifest is scanned, not only its texts: a state or a theme built from runtime data.
    def test_whole_manifest_is_scanned(self):
        for field, value in (("state", "connected from 10.0.0.5"), ("theme", "QQ0QQ's theme"),
                             ("extra", ["/home/runner/x"])):
            with self.subTest(field=field):
                p = self.t.image("linux/window.png")
                p.with_suffix(".json").write_text(json.dumps({"image": p.name, "texts": ["Amp"], field: value}),
                                                  encoding="utf-8")
                f = self.t.findings()
                self.assertTrue(f and all("window.json" in x for x in f), f)

    def test_png_without_manifest_fails_but_manual_is_exempt(self):
        self.t.image("windows/a.png", manifest=False)
        self.assertFinds("windows/a.png: no manifest")
        t2 = Tree()
        try:
            t2.image("windows/manual/uac.png", manifest=False)
            self.assertEqual(t2.findings(), [])
        finally:
            t2.close()

    def test_manifest_naming_another_image_or_empty_fails(self):
        p = self.t.image("pi/tui.png")
        p.with_suffix(".json").write_text(json.dumps({"image": "other.png", "texts": ["x"]}))
        self.assertFinds("names image 'other.png'")
        p.with_suffix(".json").write_text(json.dumps({"image": "tui.png", "texts": []}))
        self.assertFinds("no texts")

    def test_metadata_fails_even_in_manual(self):
        self.t.image("linux/manual/polkit.png", manifest=False, data=png(b"Author\x00someone"))
        self.assertFinds("carries metadata chunks ['tEXt']")

    def test_size_budget(self):
        self.t.image("linux/big.png", data=png() + b"\x00" * gate.SIZE_BUDGET)
        self.assertFinds("the budget is under")


if __name__ == "__main__":
    unittest.main()
