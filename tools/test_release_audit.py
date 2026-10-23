#!/usr/bin/env python3
"""Tests for tools/release_audit.py: each gate passes a clean input and fails the defect it exists for."""
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

TOOL = Path(__file__).resolve().parent / "release_audit.py"


def run(*args):
    r = subprocess.run([sys.executable, str(TOOL), *map(str, args)], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def tiny_elf(needed, is64=True):
    """A minimal little-endian ELF file with a .dynstr and a .dynamic listing `needed` (or no .dynamic: None)."""
    strtab = b"\0" + b"".join(n.encode() + b"\0" for n in (needed or []))
    offsets, pos = [], 1
    for n in needed or []:
        offsets.append(pos)
        pos += len(n) + 1
    dfmt = "<qQ" if is64 else "<iI"
    dyn = b"".join(struct.pack(dfmt, 1, o) for o in offsets) + struct.pack(dfmt, 0, 0)
    ehsize, shentsize = (64, 64) if is64 else (52, 40)
    str_off = ehsize
    dyn_off = str_off + len(strtab)
    sh_off = dyn_off + len(dyn)
    sfmt = "<IIQQQQIIQQ" if is64 else "<IIIIIIIIII"
    sections = [struct.pack(sfmt, *([0] * 10)), struct.pack(sfmt, 0, 3, 0, 0, str_off, len(strtab), 0, 0, 1, 0)]
    if needed is not None:
        sections.append(struct.pack(sfmt, 0, 6, 0, 0, dyn_off, len(dyn), 1, 0, 8, struct.calcsize(dfmt)))
    ident = b"\x7fELF" + bytes([2 if is64 else 1, 1, 1]) + b"\0" * 9
    if is64:
        header = ident + struct.pack("<HHIQQQIHHHHHH", 2, 62, 1, 0, 0, sh_off, 0, ehsize, 0, 0, shentsize, len(sections), 0)
    else:
        header = ident + struct.pack("<HHIIIIIHHHHHH", 2, 3, 1, 0, 0, sh_off, 0, ehsize, 0, 0, shentsize, len(sections), 0)
    return header + strtab + dyn + b"".join(sections)


def verneed_elf(versions, library="libc.so.6"):
    """A minimal little-endian ELF64 whose SHT_GNU_verneed section requires `versions` (for example GLIBC_2.38) of
    `library` (no verneed section at all when `versions` is None: a data-only library)."""
    names = [library] + list(versions or [])
    strtab = b"\0" + b"".join(n.encode() + b"\0" for n in names)
    offs, pos = {}, 1
    for n in names:
        offs[n] = pos
        pos += len(n) + 1
    vn = b""
    if versions:
        aux = b"".join(struct.pack("<IHHII", 0, 0, 2 + i, offs[v], 16 if i + 1 < len(versions) else 0)
                       for i, v in enumerate(versions))
        vn = struct.pack("<HHIII", 1, len(versions), offs[library], 16, 0) + aux
    str_off = 64
    vn_off = str_off + len(strtab)
    sh_off = vn_off + len(vn)
    sfmt = "<IIQQQQIIQQ"
    sections = [struct.pack(sfmt, *([0] * 10)), struct.pack(sfmt, 0, 3, 0, 0, str_off, len(strtab), 0, 0, 1, 0)]
    if versions:
        sections.append(struct.pack(sfmt, 0, 0x6FFFFFFE, 0, 0, vn_off, len(vn), 1, 1, 8, 0))
    ident = b"\x7fELF" + bytes([2, 1, 1]) + b"\0" * 9
    header = ident + struct.pack("<HHIQQQIHHHHHH", 3, 62, 1, 0, 0, sh_off, 0, 64, 0, 0, 64, len(sections), 1)
    return header + strtab + vn + b"".join(sections)


def stamped_elf(record, name=b".asb_stamp", stype=1):
    """A minimal ELF64 with one section `name` holding `record`, and its section-name table."""
    shstr = b"\0" + name + b"\0.shstrtab\0"
    rec_off = 64
    str_off = rec_off + len(record)
    sh_off = str_off + len(shstr)
    sfmt = "<IIQQQQIIQQ"
    sections = [struct.pack(sfmt, *([0] * 10)),
                struct.pack(sfmt, 1, stype, 0, 0, rec_off, len(record), 0, 0, 1, 0),
                struct.pack(sfmt, 2 + len(name), 3, 0, 0, str_off, len(shstr), 0, 0, 1, 0)]
    ident = b"\x7fELF" + bytes([2, 1, 1]) + b"\0" * 9
    header = ident + struct.pack("<HHIQQQIHHHHHH", 2, 62, 1, 0, 0, sh_off, 0, 64, 0, 0, 64, len(sections), 2)
    return header + record + shstr + b"".join(sections)


def stamped_pe(record, name=b".asbstmp"):
    dos = b"MZ" + b"\0" * 0x3A + struct.pack("<I", 0x40)
    coff = b"PE\0\0" + struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, 0, 0x22)
    section = name.ljust(8, b"\0") + struct.pack("<IIIIIIHHI", 256, 0x1000, 0x200, 0x200, 0, 0, 0, 0, 0)
    head = (dos + coff + section).ljust(0x200, b"\0")
    return head + record.ljust(0x200, b"\0")


def stamp_record(version="1.1.0", magic=b"ASB-BUILD-STAMP\0"):
    body = json.dumps({"schema": 1, "product": "aether-serial-bridge", "version": version}, separators=(",", ":"))
    return (magic + body.encode()).ljust(256, b"\0")


class ReleaseAudit(unittest.TestCase):
    def setUp(self):
        self.d = Path(tempfile.mkdtemp())

    def file(self, name, data):
        p = self.d / name
        p.write_bytes(data)
        return p

    def test_seams(self):
        clean = self.file("clean", b"\x7fELF ... --status --install ...")
        self.assertEqual(run("seams", clean)[0], 0)
        for marker in (b"ASB_TEST_PAUSE", b"--test-op", b"--identity-probe", "ASB_TEST_FAIL".encode("utf-16-le")):
            bad = self.file("bad", b"xx" + marker + b"yy")
            rc, out = run("seams", bad)
            self.assertEqual(rc, 1, out)
        # The control: a file without markers fails it, one with markers passes it.
        self.assertEqual(run("seams-control", clean)[0], 1)
        self.assertEqual(run("seams-control", self.file("seamy", b"--test-op"))[0], 0)

    def test_digest(self):
        headless = self.file("h.exe", b"headless bytes")
        digest = hashlib.sha256(b"headless bytes").hexdigest().encode()
        self.assertEqual(run("digest", self.file("g.exe", b"gui " + digest + b" gui"), headless)[0], 0)
        other = hashlib.sha256(b"another build").hexdigest().encode()
        rc, out = run("digest", self.file("g2.exe", b"gui " + other + b" gui"), headless)
        self.assertEqual(rc, 1, out)

    def test_zip(self):
        z = self.d / "a.zip"
        with zipfile.ZipFile(z, "w") as f:
            f.writestr("A.exe", "a")
            f.writestr("b.exe", "b")
        self.assertEqual(run("zip", z, "A.exe", "b.exe")[0], 0)
        self.assertEqual(run("zip", z, "A.exe", "b.exe", "README.txt")[0], 1)
        with zipfile.ZipFile(z, "a") as f:
            f.writestr("Qt6Core.dll", "x")
        self.assertEqual(run("zip", z, "A.exe", "b.exe", "Qt6Core.dll")[0], 1)   # a DLL is never allowed
        z2 = self.d / "b.zip"
        with zipfile.ZipFile(z2, "w") as f:
            f.writestr("A.exe", "a")
            f.writestr("plugins/platforms/qwindows.dll", "x")
        self.assertEqual(run("zip", z2, "A.exe", "plugins/platforms/qwindows.dll")[0], 1)

    def test_needed_allowlist(self):
        allow = ["libc.so.6", "libm.so.6", "ld-linux-*"]
        ok = self.file("ok", tiny_elf(["libm.so.6", "libc.so.6", "ld-linux-aarch64.so.1"]))
        self.assertEqual(run("needed", ok, *allow)[0], 0)
        self.assertEqual(run("needed", self.file("ok32", tiny_elf(["libc.so.6"], is64=False)), *allow)[0], 0)
        # A library outside the list fails, whatever it is (an allowlist, not a denylist of known names).
        for extra in ("libzstd.so.1", "libsystemd.so.0", "libQt6Core.so.6", "libstdc++.so.6"):
            rc, out = run("needed", self.file("bad", tiny_elf(["libc.so.6", extra])), *allow)
            self.assertEqual(rc, 1, out)
            self.assertIn(extra, out)
        # Not an ELF, or no dynamic section, or nothing needed at all: never a pass.
        self.assertEqual(run("needed", self.file("text", b"not an elf"), *allow)[0], 1)
        self.assertEqual(run("needed", self.file("nodyn", tiny_elf(None)), *allow)[0], 1)
        self.assertEqual(run("needed", self.file("empty", tiny_elf([])), *allow)[0], 1)
        # The control: passes only on a file the gate rejects.
        self.assertEqual(run("needed-control", self.file("qt", tiny_elf(["libQt6Core.so.6", "libc.so.6"])), *allow)[0], 0)
        self.assertEqual(run("needed-control", ok, *allow)[0], 1)

    def test_gui_report(self):
        good = self.file("good", b'Qt says something\n{"exposed":true,"painted":true,"modules":["a"]}\n')
        self.assertEqual(run("gui-report", good, "0")[0], 0)
        # The exit status is part of the contract: a good report followed by a failed exit, a crash in teardown
        # (128+signal, as timeout reports it) or a hang (timeout's 124, or 137 after its -k) fails.
        for status, says in (("1", "exited 1"), ("124", "timed out"), ("137", "signal 9"), ("139", "signal 11")):
            rc, out = run("gui-report", good, status)
            self.assertEqual(rc, 1, out)
            self.assertIn(says, out)
        # The exit status is required and must be a number (fail closed).
        self.assertEqual(run("gui-report", good)[0], 2)
        self.assertEqual(run("gui-report", good, "")[0], 1)
        self.assertEqual(run("gui-report", good, "x")[0], 1)
        # The report is the LAST JSON line (an earlier brace-led line is not it).
        later = self.file("later", b'{"exposed":false}\n{"exposed":true,"painted":true,"modules":[]}\n')
        self.assertEqual(run("gui-report", later, "0")[0], 0)
        earlier = self.file("earlier", b'{"exposed":true,"painted":true}\n{"exposed":false,"painted":false}\n')
        self.assertEqual(run("gui-report", earlier, "0")[0], 1)
        for body in (b'{"exposed":false,"painted":true,"modules":[]}\n', b'{"exposed":true,"painted":false}\n',
                     b'{"exposed":"true","painted":true}\n', b"no report at all\n", b"{garbled\n"):
            rc, out = run("gui-report", self.file("bad", body), "0")
            self.assertEqual(rc, 1, out)
        self.assertEqual(run("gui-report", self.d / "missing", "0")[0], 1)

    # v1.1 INV-121: the Linux window is RUN with --print-program-digest; the digest it prints must be the embedded
    # program's and the release program's. The control fails on a window that carries another digest.
    def test_digest_runs_the_linux_window(self):
        embedded = self.file("embedded", b"release program bytes")
        release = self.file("release", b"release program bytes")
        want = hashlib.sha256(b"release program bytes").hexdigest()

        def apprun(name, printed, rc=0):
            """An AppImage's AppRun (in its own directory) that prints `printed` for --print-program-digest."""
            (self.d / name).mkdir()
            p = self.d / name / "AppRun"
            p.write_text("#!/bin/sh\n[ \"$1\" = --print-program-digest ] || exit 9\necho %s\nexit %d\n" % (printed, rc))
            p.chmod(0o755)
            return p
        good = apprun("good", want)
        self.assertEqual(run("digest", good, embedded, release)[0], 0)
        self.assertEqual(run("digest", good, embedded)[0], 0)
        self.assertEqual(run("digest-control", good, embedded, release)[0], 1)
        dev = apprun("dev", hashlib.sha256(b"a development program").hexdigest())
        rc, out = run("digest", dev, embedded, release)
        self.assertEqual(rc, 1, out)
        self.assertEqual(run("digest-control", dev, embedded, release)[0], 0)
        # The embedded program is not the release program: the window agrees with it, the release does not.
        self.assertEqual(run("digest", good, embedded, self.file("other", b"another release"))[0], 1)
        for broken in (apprun("fails", want, rc=1), apprun("garbled", "not-a-digest")):
            self.assertEqual(run("digest", broken, embedded, release)[0], 1)
            # The control fails closed: a control window that does not run proves nothing.
            rc, out = run("digest-control", broken, embedded, release)
            self.assertEqual(rc, 1, out)
            self.assertIn("not exercised", out)
        # An ELF window is run, never searched: one that cannot run fails, the gate and its control alike.
        elf = self.file("elf-window", b"\x7fELF" + want.encode())
        self.assertEqual(run("digest", elf, embedded, release)[0], 1)
        self.assertEqual(run("digest-control", elf, embedded, release)[0], 1)
        # A release mismatch alone is not the control's subject: the window still carries the embedded digest.
        self.assertEqual(run("digest-control", good, embedded, self.file("other2", b"another release"))[0], 1)

    # v1.1 INV-121: every ELF file in the tree, plugins included; data-only libraries pass; a tree with no GLIBC_
    # requirement at all, or an unreadable ELF, fails; links are not followed.
    def test_glibc_tree(self):
        root = self.d / "squashfs-root"
        for sub, versions in (("usr/bin/aether-serial-bridge-gui", ["GLIBC_2.34", "GLIBC_2.2.5"]),
                              ("usr/lib/libQt6Core.so.6", ["GLIBC_2.35"]), ("usr/lib/libicudata.so.73", None),
                              ("usr/libexec/aether-serial-bridge", ["GLIBC_2.17"]),
                              ("usr/plugins/platforms/libqxcb.so", ["GLIBC_2.14"])):
            p = root / sub
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(verneed_elf(versions))
        (root / "usr/share").mkdir(parents=True)
        (root / "usr/share/readme.txt").write_text("not an ELF file")
        os.symlink("/usr/lib/x86_64-linux-gnu/libc.so.6", root / "usr/lib/libc-link.so")
        rc, out = run("glibc-tree", root, "2.35")
        self.assertEqual(rc, 0, out)
        self.assertIn("GLIBC_2.35", out)
        self.assertEqual(run("glibc-tree-control", root, "2.35")[0], 1)
        self.assertEqual(run("glibc-tree", root, "2.34")[0], 1)
        # A planted newer-glibc plugin (the release control) fails the walk, and passes the control.
        plant = root / "usr/plugins/imageformats/libplanted.so"
        plant.parent.mkdir(parents=True)
        plant.write_bytes(verneed_elf(["GLIBC_2.38", "GLIBC_2.34"]))
        rc, out = run("glibc-tree", root, "2.35")
        self.assertEqual(rc, 1, out)
        self.assertIn("usr/plugins/imageformats/libplanted.so needs GLIBC_2.38", out)
        self.assertEqual(run("glibc-tree-control", root, "2.35")[0], 0)
        plant.unlink()
        # An ELF file whose section table is cut off is a failure, never skipped.
        (root / "usr/lib/libcut.so").write_bytes(verneed_elf(["GLIBC_2.17"])[:80])
        self.assertEqual(run("glibc-tree", root, "2.35")[0], 1)
        # Nothing needs glibc at all: the audit would prove nothing.
        bare = self.d / "bare"
        (bare / "usr/lib").mkdir(parents=True)
        (bare / "usr/lib/libdata.so").write_bytes(verneed_elf(None))
        self.assertEqual(run("glibc-tree", bare, "2.35")[0], 1)
        self.assertEqual(run("glibc-tree", self.d / "empty-dir-missing", "2.35")[0], 1)

    # The parser agrees with objdump -T (GlibcAudit.cmake's reading) on a real library, where objdump exists.
    def test_glibc_needs_match_objdump(self):
        objdump = shutil.which("objdump")
        lib = next((p for p in ("/usr/lib/x86_64-linux-gnu/libstdc++.so.6", "/usr/lib/aarch64-linux-gnu/libstdc++.so.6")
                    if os.path.exists(p)), None)
        if not objdump or not lib:
            self.skipTest("no objdump or no libstdc++ here")
        sys.path.insert(0, str(TOOL.parent))
        import release_audit
        with open(lib, "rb") as f:
            ours = max(release_audit.elf_glibc_needs(f.read()), key=release_audit.version_key)
        out = subprocess.run([objdump, "-T", lib], capture_output=True, text=True).stdout
        theirs = max(set(__import__("re").findall(r"GLIBC_(\d+(?:\.\d+)+)", out)), key=release_audit.version_key)
        self.assertEqual(ours, theirs)

    # v1.1 fix (the window on stock Ubuntu): every NEEDED is bundled in usr/lib or on the host allowlist.
    def test_host_libs(self):
        root = self.d / "squashfs-root"
        allow = self.d / "allow.txt"
        allow.write_text("# stock desktop\nlibc.so.6\nld-linux-*  # both arches\nlibGL.so.1\n\nlibxcb.so.1\n")
        tree = {"usr/bin/aether-serial-bridge-gui": ["libQt6Gui.so.6", "libc.so.6", "ld-linux-x86-64.so.2"],
                "usr/lib/libQt6Gui.so.6": ["libGL.so.1", "libxcb.so.1", "libc.so.6"],
                "usr/plugins/platforms/libqxcb.so": ["libQt6Gui.so.6", "libc.so.6"],
                "usr/libexec/aether-serial-bridge": None}   # a static ELF file needs nothing
        for sub, needed in tree.items():
            p = root / sub
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(tiny_elf(needed))
        os.symlink("libQt6Gui.so.6", root / "usr/lib/libQt6Gui.so")   # a link is a bundled name, not walked
        (root / "usr/share").mkdir(parents=True)
        (root / "usr/share/readme.txt").write_text("not an ELF file")
        rc, out = run("host-libs", root, allow)
        self.assertEqual(rc, 0, out)
        self.assertEqual(run("host-libs-control", root, allow)[0], 1)
        # The bug: the window needs libOpenGL.so.0, neither bundled nor on the list. The gate names it; the control
        # passes on exactly this tree.
        gui = root / "usr/bin/aether-serial-bridge-gui"
        gui.write_bytes(tiny_elf(["libQt6Gui.so.6", "libOpenGL.so.0", "libc.so.6"]))
        rc, out = run("host-libs", root, allow)
        self.assertEqual(rc, 1, out)
        self.assertIn("libOpenGL.so.0 is neither bundled nor a stock-desktop host library", out)
        self.assertIn("usr/bin/aether-serial-bridge-gui", out)
        rc, out = run("host-libs-control", root, allow)
        self.assertEqual(rc, 0, out)
        # A plugin's unbundled library fails as well (the walk reaches usr/plugins), and a library bundled only
        # outside usr/lib is not bundled (the RUNPATHs point at usr/lib).
        gui.write_bytes(tiny_elf(["libQt6Gui.so.6", "libc.so.6"]))
        (root / "usr/plugins/platforms/libqxcb.so").write_bytes(tiny_elf(["libxkbcommon-x11.so.0", "libc.so.6"]))
        (root / "usr/plugins/libxkbcommon-x11.so.0").write_bytes(tiny_elf(["libc.so.6"]))
        rc, out = run("host-libs", root, allow)
        self.assertEqual(rc, 1, out)
        self.assertIn("libxkbcommon-x11.so.0", out)
        (root / "usr/plugins/libxkbcommon-x11.so.0").unlink()
        (root / "usr/lib/libxkbcommon-x11.so.0").write_bytes(tiny_elf(["libc.so.6"]))
        self.assertEqual(run("host-libs", root, allow)[0], 0)
        # A bundled name must resolve to a regular file inside the tree. A valid relative link to
        # a real file is bundled; a dangling link and a link escaping the tree (to a real host file) are not, and the
        # NEEDED that names them fails with the reason.
        lib = root / "usr/lib"
        (lib / "libxkb.so.0.0.0").write_bytes(tiny_elf(["libc.so.6"]))
        (lib / "libxkbcommon-x11.so.0").unlink()   # the regular file above becomes a relative link to the real one
        os.symlink("libxkb.so.0.0.0", lib / "libxkbcommon-x11.so.0")
        (root / "usr/plugins/platforms/libqxcb.so").write_bytes(tiny_elf(["libxkbcommon-x11.so.0", "libdangle.so.1",
                                                                          "libc.so.6"]))
        os.symlink("libgone.so.1.2", lib / "libdangle.so.1")
        rc, out = run("host-libs", root, allow)
        self.assertEqual(rc, 1, out)
        self.assertIn("usr/lib/libdangle.so.1 is a dangling link", out)
        self.assertNotIn("libxkbcommon-x11.so.0 is", out)   # the valid relative link is bundled
        self.assertEqual(run("host-libs-control", root, allow)[0], 0)
        (lib / "libdangle.so.1").unlink()
        outside = self.d / "host-lib.so.1"
        outside.write_bytes(tiny_elf(["libc.so.6"]))
        os.symlink(os.path.relpath(outside, lib), lib / "libdangle.so.1")   # resolves, but outside the tree
        rc, out = run("host-libs", root, allow)
        self.assertEqual(rc, 1, out)
        self.assertIn("usr/lib/libdangle.so.1 is a link that escapes the image", out)
        (lib / "libdangle.so.1").unlink()
        os.symlink(str(outside), lib / "libdangle.so.1")   # an absolute escaping link too
        self.assertEqual(run("host-libs", root, allow)[0], 1)
        (lib / "libdangle.so.1").unlink()
        # A broken link is refused even under an allowlisted name: the image claims a copy it does not carry.
        os.symlink("libxcb-gone.so.1", lib / "libxcb.so.1")
        rc, out = run("host-libs", root, allow)
        self.assertEqual(rc, 1, out)
        self.assertIn("usr/lib/libxcb.so.1 is a dangling link", out)
        (lib / "libxcb.so.1").unlink()
        os.symlink("libxkb.so.0.0.0", lib / "libdangle.so.1")   # now a valid link: the tree passes
        rc, out = run("host-libs", root, allow)
        self.assertEqual(rc, 0, out)
        (root / "usr/plugins/platforms/libqxcb.so").write_bytes(tiny_elf(["libxkbcommon-x11.so.0", "libc.so.6"]))
        (lib / "libdangle.so.1").unlink()
        # An allowlist that admits libOpenGL.so.0 (by name or by glob) is refused, even when nothing needs it.
        for line in ("libOpenGL.so.0", "libOpenGL*", "lib*GL*"):
            bad_allow = self.d / "bad-allow.txt"
            bad_allow.write_text(allow.read_text() + line + "\n")
            rc, out = run("host-libs", root, bad_allow)
            self.assertEqual(rc, 1, (line, out))
            self.assertIn("admits libOpenGL.so.0", out)
        # A bundled GLVND library is refused: the host's driver stack must provide them.
        for name in ("libOpenGL.so.0", "libGLdispatch.so.0", "libGL.so.1", "libGLX.so.0", "libEGL.so.1"):
            planted = root / "usr/lib" / name
            planted.write_bytes(tiny_elf(["libc.so.6"]))
            rc, out = run("host-libs", root, allow)
            self.assertEqual(rc, 1, (name, out))
            self.assertIn("usr/lib/%s is bundled" % name, out)
            planted.unlink()
        self.assertEqual(run("host-libs", root, allow)[0], 0)
        # An ELF file whose dynamic section cannot be read is a failure, never skipped.
        cut = root / "usr/lib/libcut.so"
        whole = tiny_elf(["libc.so.6"])
        cut.write_bytes(whole[:-8])
        self.assertEqual(run("host-libs", root, allow)[0], 1)
        cut.unlink()
        # Fail closed: an empty or missing allowlist, no ELF file, or nothing needed at all.
        empty = self.d / "empty.txt"
        empty.write_text("# nothing\n")
        self.assertEqual(run("host-libs", root, empty)[0], 1)
        closed = self.d / "closed"   # everything bundled, so only the empty-allowlist check can refuse it
        (closed / "usr/bin").mkdir(parents=True)
        (closed / "usr/lib").mkdir(parents=True)
        (closed / "usr/bin/app").write_bytes(tiny_elf(["libfoo.so.1"]))
        (closed / "usr/lib/libfoo.so.1").write_bytes(tiny_elf(None))
        self.assertEqual(run("host-libs", closed, allow)[0], 0)
        rc, out = run("host-libs", closed, empty)
        self.assertEqual(rc, 1, out)
        self.assertIn("is empty", out)
        self.assertEqual(run("host-libs", root, self.d / "missing.txt")[0], 1)
        bare = self.d / "bare"
        (bare / "usr/lib").mkdir(parents=True)
        (bare / "usr/lib/readme").write_text("x")
        self.assertEqual(run("host-libs", bare, allow)[0], 1)
        (bare / "usr/bin").mkdir()
        (bare / "usr/bin/static").write_bytes(tiny_elf(None))
        rc, out = run("host-libs", bare, allow)
        self.assertEqual(rc, 1, out)
        self.assertIn("needs any library", out)

    # The shipped allowlist itself: libOpenGL.so.0 and libGLX.so.0 are not on it.
    def test_host_libs_shipped_allowlist(self):
        sys.path.insert(0, str(TOOL.parent))
        import release_audit
        import fnmatch
        allowed = release_audit.read_allowlist(TOOL.parent.parent / "packaging/linux/appimage/host-libs.allowlist.txt")
        self.assertIn("libGL.so.1", allowed)
        for lib in ("libOpenGL.so.0", "libGLX.so.0", "libGLdispatch.so.0"):
            self.assertFalse([a for a in allowed if fnmatch.fnmatchcase(lib, a)], lib)

    # v1.1 D61: the stamp gate reads the section through the section table, ELF and PE.
    def test_stamp(self):
        self.assertEqual(run("stamp", self.file("e", stamped_elf(stamp_record("1.1.0"))), "1.1.0")[0], 0)
        self.assertEqual(run("stamp", self.file("p.exe", stamped_pe(stamp_record("1.1.0"))), "1.1.0")[0], 0)
        rc, out = run("stamp", self.file("e2", stamped_elf(stamp_record("1.0.9"))), "1.1.0")
        self.assertEqual(rc, 1, out)
        self.assertEqual(run("stamp", self.file("p2.exe", stamped_pe(stamp_record("1.1.0"), name=b".rdata")), "1.1.0")[0], 1)
        self.assertEqual(run("stamp", self.file("e3", stamped_elf(stamp_record(), name=b".rodata")), "1.1.0")[0], 1)
        self.assertEqual(run("stamp", self.file("e4", stamped_elf(stamp_record(magic=b"X" * 16))), "1.1.0")[0], 1)
        self.assertEqual(run("stamp", self.file("e5", stamped_elf(stamp_record()[:100])), "1.1.0")[0], 1)
        self.assertEqual(run("stamp", self.file("e6", stamped_elf(stamp_record(), stype=8)), "1.1.0")[0], 1)
        # A v1.0 program: the record's bytes elsewhere in the file are not a stamp (no scanning).
        self.assertEqual(run("stamp", self.file("v10", tiny_elf(["libc.so.6"]) + stamp_record()), "1.1.0")[0], 1)
        self.assertEqual(run("stamp", self.file("text", b"not a program"), "1.1.0")[0], 1)

    def test_same_and_sums(self):
        a = self.file("a", b"1")
        self.assertEqual(run("same", a, self.file("b", b"1"))[0], 0)
        self.assertEqual(run("same", a, self.file("c", b"2"))[0], 1)
        out = self.d / "SHA256SUMS"
        self.assertEqual(run("sha256sums", self.d, out)[0], 0)
        lines = out.read_text().splitlines()
        self.assertIn("%s  a" % hashlib.sha256(b"1").hexdigest(), lines)
        self.assertNotIn("SHA256SUMS", " ".join(lines))


if __name__ == "__main__":
    unittest.main()
