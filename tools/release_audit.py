#!/usr/bin/env python3
"""Release artifact gates that need no toolchain (WS-13). Each subcommand prints what it checked and exits 1 on a
finding, 2 on a usage error.

  seams <exe>...                  no test seam reached the release build: none of the seam markers (ASB_TEST_*
                                  variables, --test-op, the identity probes) occurs in the file (master section 4.11,
                                  WS-7 item 10: "WS-13's release audit asserts they are absent").
  seams-control <exe>...          the same scan must FIND markers in a test build (the audit's positive control:
                                  a scanner that finds nothing anywhere would pass every release).
  digest <gui-exe> <headless-exe> [<release-exe>]
                                  the SHA-256 compiled into the GUI (HeadlessDigest.h, WS-7 install copy) is the
                                  digest of the headless executable shipped beside it; Save & apply refuses any
                                  other bytes, so a mismatch is a release that cannot install. A Windows GUI (PE) is
                                  searched for the digest. Anything else is RUN with --print-program-digest (the
                                  Linux window, or the AppImage's AppRun; v1.1 INV-121): the digest it prints must be
                                  the headless file's (the program the AppImage embeds) and, when given, the release
                                  program's.
  digest-control <same arguments> the same check must FAIL (a window configured without the release program's digest).
  zip <zip> <name>...             the zip holds exactly these top-level files: no folder, no DLL, nothing else.
  same <file> <file>              byte-identical (the AppImage's embedded program is the released Linux binary).
  sha256sums <dir> <out>          writes SHA256SUMS for every regular file in <dir> (sorted, `sha256sum -c` format).
  needed <elf> <allowed>...       an ALLOWLIST of the ELF file's DT_NEEDED entries (fnmatch globs, for example
                                  libc.so.6 libm.so.6 'ld-linux-*'): any other shared library fails, and so does a
                                  file that is not a dynamically linked ELF (an empty list would prove nothing).
                                  Read from the file itself (ELF64/ELF32, either byte order); no readelf needed.
  needed-control <elf> <allowed>...  the same check must FAIL on this file (the gate's positive control).
  glibc-tree <root> <max>         every ELF file under <root> (regular files identified by their magic; links are not
                                  followed) needs at most GLIBC_<max>, read from its version-needs section (v1.1
                                  INV-121: the extracted AppImage, usr/bin, usr/lib, usr/plugins, usr/libexec). A file
                                  with no GLIBC_ requirement (a data-only library) is fine; a tree with none at all,
                                  or an ELF file whose sections cannot be read, fails. Prints the worst offender.
  glibc-tree-control <root> <max> the same walk must FAIL (a tree with a planted newer-glibc ELF file).
  host-libs <root> <allowlist>    every DT_NEEDED of every ELF file under <root> (the extracted AppImage) is either
                                  BUNDLED (a name in <root>/usr/lib, where the RUNPATHs point, that resolves to a
                                  regular file inside <root>: a dangling or escaping link is not bundled) or
                                  matches a glob in <allowlist> (a file, one glob per line, # comments): the host
                                  libraries a stock Ubuntu desktop installs (v1.1 fix: the window NEEDed
                                  libOpenGL.so.0, which neither the image nor a stock desktop has, and did not start).
                                  An allowlist that admits a NEVER_ON_HOST library, a bundled GLVND dispatch library
                                  (they must come from the host's driver stack), a tree with no ELF file, or one in
                                  which nothing needs anything, fails.
  host-libs-control <root> <allowlist>
                                  the same walk must FAIL (a copy of the tree whose window was given
                                  `patchelf --add-needed libOpenGL.so.0`: tools/host-libs-control.sh).
  stamp <exe> <version>           the executable carries the build stamp (v1.1 D61) of <version>: the .asb_stamp
                                  (ELF) or .asbstmp (PE) section, found through the section table, holding the magic
                                  and {"schema":1,"product":"aether-serial-bridge","version":<version>}.
  gui-report <file> <exit-status> the window's --self-test-init (run from the AppImage, offscreen, under timeout): it must
                                  exit 0 (not 124, a timeout, nor 128+N, a signal) AND its last JSON line must report
                                  exposed AND painted (a missing or garbled report fails).
"""
import json
import fnmatch
import struct
import hashlib
import os
import re
import sys
import zipfile

# Strings only a test build carries: the seam variables' common prefix and the test-only command-line switches.
SEAM_MARKERS = [b"ASB_TEST_", b"--test-op", b"--identity-probe", b"--probe-path-before-identity"]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def markers_in(path):
    data = open(path, "rb").read()
    found = []
    for m in SEAM_MARKERS:
        # ASCII and UTF-16LE (Windows string literals).
        for enc in (m, m.decode().encode("utf-16-le")):
            if enc in data:
                found.append(m.decode() + (" (utf-16)" if enc is not m else ""))
    return found


def cmd_seams(paths, control=False):
    bad = 0
    for p in paths:
        found = markers_in(p)
        if control:
            if not found:
                print("FAIL %s: the positive control found no seam marker in a test build" % p)
                bad = 1
            else:
                print("ok   %s: control finds %s" % (p, ", ".join(found)))
        elif found:
            print("FAIL %s: test seam markers in a release executable: %s" % (p, ", ".join(found)))
            bad = 1
        else:
            print("ok   %s: no seam marker (%s)" % (p, ", ".join(m.decode() for m in SEAM_MARKERS)))
    return bad


def cmd_digest(gui, headless):
    data = open(gui, "rb").read()
    want = sha256(headless)
    candidates = sorted(set(m.decode() for m in re.findall(rb"(?<![0-9a-f])[0-9a-f]{64}(?![0-9a-f])", data)))
    if want.encode() in data:
        print("ok   %s carries the SHA-256 of %s: %s" % (os.path.basename(gui), os.path.basename(headless), want))
        return 0
    print("FAIL %s does not carry the SHA-256 of %s (%s); 64-hex strings in it: %s"
          % (os.path.basename(gui), os.path.basename(headless), want, candidates[:10]))
    return 1


def program_digest(runnable):
    """What `<runnable> --print-program-digest` prints (one 64-hex line), or None with the reason."""
    import subprocess
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    try:
        r = subprocess.run([runnable, "--print-program-digest"], capture_output=True, text=True, timeout=120, env=env)
    except (OSError, subprocess.TimeoutExpired) as e:
        return None, "could not run %s --print-program-digest: %s" % (runnable, e)
    lines = [l.strip() for l in r.stdout.splitlines() if l.strip()]
    if r.returncode != 0 or len(lines) != 1 or not re.fullmatch(r"[0-9a-f]{64}", lines[0]):
        return None, "%s --print-program-digest: exit %d, output %r %r" % (runnable, r.returncode, r.stdout[-300:], r.stderr[-300:])
    return lines[0], None


def cmd_digest_elf(gui, headless, release=None, control=False):
    compiled, why = program_digest(gui)
    want = sha256(headless)
    bad = []
    if compiled is None:
        bad.append(why)
    elif compiled != want:
        bad.append("the window's compiled digest %s is not the SHA-256 of %s (%s)" % (compiled, headless, want))
    if release is not None and sha256(release) != want:
        bad.append("%s (%s) is not the release program %s (%s)" % (headless, want, release, sha256(release)))
    if control:
        # Fail closed: the control passes only when the window RAN, printed a digest, and that digest
        # is not the embedded program's. A window that cannot run, or a release mismatch, proves nothing about the
        # digest check itself.
        if compiled is None:
            print("FAIL control: the control window did not report a digest, so the gate was not exercised: %s" % why)
            return 1
        if compiled == want:
            print("FAIL control: %s carries the digest of %s; the gate would pass anything" % (gui, headless))
            return 1
        print("ok   control: the gate refuses it: the window's compiled digest %s is not the SHA-256 of %s (%s)"
              % (compiled, headless, want))
        return 0
    if bad:
        for b in bad:
            print("FAIL %s" % b)
        return 1
    print("ok   %s compiled digest %s == sha256(%s)%s" % (gui, compiled, headless,
                                                        " == sha256(%s)" % release if release else ""))
    return 0


def cmd_zip(path, names):
    with zipfile.ZipFile(path) as z:
        entries = z.namelist()
    want = sorted(names)
    got = sorted(entries)
    print("zip %s: %s" % (os.path.basename(path), got))
    bad = 0
    if got != want:
        print("FAIL the zip holds %s, expected exactly %s" % (got, want))
        bad = 1
    dlls = [e for e in entries if e.lower().endswith(".dll") or "/" in e.rstrip("/") or e.endswith("/")]
    if dlls:
        print("FAIL folders or DLLs in the zip: %s" % dlls)
        bad = 1
    if not bad:
        print("ok   exactly %d files, no folder, no DLL" % len(entries))
    return bad


def cmd_same(a, b):
    ha, hb = sha256(a), sha256(b)
    if ha == hb:
        print("ok   %s == %s (%s)" % (a, b, ha))
        return 0
    print("FAIL %s (%s) != %s (%s)" % (a, ha, b, hb))
    return 1


def cmd_sha256sums(d, out):
    files = sorted(f for f in os.listdir(d) if os.path.isfile(os.path.join(d, f)) and f != os.path.basename(out))
    if not files:
        print("FAIL nothing to sum in %s" % d)
        return 1
    with open(out, "w", newline="\n") as f:
        for name in files:
            line = "%s  %s" % (sha256(os.path.join(d, name)), name)
            f.write(line + "\n")
            print(line)
    return 0


def elf_needed(path):
    """The DT_NEEDED names of an ELF file, or None when it is not a dynamically linked ELF."""
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or len(data) < 64:
        return None
    is64 = data[4] == 2
    end = "<" if data[5] == 1 else ">"
    if is64:
        shoff, = struct.unpack_from(end + "Q", data, 0x28)
        shentsize, shnum = struct.unpack_from(end + "HH", data, 0x3A)
    else:
        shoff, = struct.unpack_from(end + "I", data, 0x20)
        shentsize, shnum = struct.unpack_from(end + "HH", data, 0x2E)
    sections = []
    for i in range(shnum):
        o = shoff + i * shentsize
        if is64:
            _name, stype, _flags, _addr, off, size, link, _info, _align, entsize = struct.unpack_from(end + "IIQQQQIIQQ", data, o)
        else:
            _name, stype, _flags, _addr, off, size, link, _info, _align, entsize = struct.unpack_from(end + "IIIIIIIIII", data, o)
        sections.append((stype, off, size, link, entsize))
    dynamic = [s for s in sections if s[0] == 6]   # SHT_DYNAMIC
    if not dynamic:
        return None
    stype, off, size, link, entsize = dynamic[0]
    _st, stroff, strsize, _l, _e = sections[link]
    fmt = end + ("qQ" if is64 else "iI")
    step = struct.calcsize(fmt)
    names = []
    for o in range(off, off + size, step):
        tag, val = struct.unpack_from(fmt, data, o)
        if tag == 0:
            break
        if tag == 1:   # DT_NEEDED
            raw = data[stroff + val:stroff + strsize]
            names.append(raw.split(b"\0", 1)[0].decode())
    return names


def cmd_needed(path, allowed, control=False):
    names = elf_needed(path)
    if names is None:
        print("%s %s: not a dynamically linked ELF file" % ("ok  " if control else "FAIL", path))
        return 0 if control else 1
    bad = [n for n in names if not any(fnmatch.fnmatchcase(n, a) for a in allowed)]
    print("%s NEEDED %s (allowed: %s)" % (os.path.basename(path), names, " ".join(allowed)))
    if control:
        if bad:
            print("ok   control: %s needs %s, outside the allowlist" % (os.path.basename(path), bad))
            return 0
        print("FAIL control: %s passed the allowlist; the gate would pass anything" % path)
        return 1
    if bad or not names:
        print("FAIL %s needs %s" % (path, bad or "nothing (not what a dynamically linked release binary looks like)"))
        return 1
    print("ok   %s needs only allowed libraries" % os.path.basename(path))
    return 0


GLIBC_RE = re.compile(r"^GLIBC_(\d+(?:\.\d+)+)$")
SHT_GNU_VERNEED = 0x6FFFFFFE


def version_key(v):
    return tuple(int(x) for x in v.split("."))


def elf_sections(data):
    """(is64, endian, [(name_offset, type, offset, size, link, info)]) of an ELF image, or None when its section
    header table cannot be read (bounds-checked)."""
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[4] not in (1, 2) or data[5] not in (1, 2):
        return None
    is64 = data[4] == 2
    end = "<" if data[5] == 1 else ">"
    if is64:
        shoff, = struct.unpack_from(end + "Q", data, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from(end + "HHH", data, 0x3A)
    else:
        shoff, = struct.unpack_from(end + "I", data, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from(end + "HHH", data, 0x2E)
    if shnum == 0 or shentsize != (64 if is64 else 40) or shoff + shnum * shentsize > len(data):
        return None
    out = []
    for i in range(shnum):
        o = shoff + i * shentsize
        if is64:
            name, stype, _f, _a, off, size, link, info, _al, _es = struct.unpack_from(end + "IIQQQQIIQQ", data, o)
        else:
            name, stype, _f, _a, off, size, link, info, _al, _es = struct.unpack_from(end + "IIIIIIIIII", data, o)
        out.append((name, stype, off, size, link, info))
    return is64, end, out, shstrndx


def elf_glibc_needs(data):
    """The GLIBC_ versions an ELF image requires (from SHT_GNU_verneed), or None when its sections cannot be read."""
    parsed = elf_sections(data)
    if parsed is None:
        return None
    _is64, end, sections, _ = parsed
    found = set()
    for _name, stype, off, size, link, info in sections:
        if stype != SHT_GNU_VERNEED:
            continue
        if link >= len(sections) or off + size > len(data):
            return None
        _n, _t, stroff, strsize, _l, _i = sections[link]
        if stroff + strsize > len(data):
            return None
        strtab = data[stroff:stroff + strsize]
        entry = off
        for _ in range(max(info, 0) or 4096):
            if entry + 16 > off + size:
                return None
            _ver, cnt, _file, aux, nxt = struct.unpack_from(end + "HHIII", data, entry)
            a = entry + aux
            for _ in range(cnt):
                if a + 16 > off + size:
                    return None
                _h, _fl, _ot, name, anext = struct.unpack_from(end + "IHHII", data, a)
                n = strtab[name:].split(b"\0", 1)[0].decode("ascii", "replace")
                m = GLIBC_RE.match(n)
                if m:
                    found.add(m.group(1))
                if anext == 0:
                    break
                a += anext
            if nxt == 0:
                break
            entry += nxt
    return found


def cmd_glibc_tree(root, limit, control=False):
    elves, unreadable, offenders = [], [], []
    worst = None
    any_glibc = False
    for d, dirs, files in os.walk(root, followlinks=False):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(d, f)
            if os.path.islink(p) or not os.path.isfile(p):
                continue
            with open(p, "rb") as fh:
                if fh.read(4) != b"\x7fELF":
                    continue
            data = open(p, "rb").read()
            elves.append(p)
            needs = elf_glibc_needs(data)
            if needs is None:
                unreadable.append(p)
                continue
            if not needs:
                continue
            any_glibc = True
            top = max(needs, key=version_key)
            if worst is None or version_key(top) > version_key(worst[0]):
                worst = (top, p)
            if version_key(top) > version_key(limit):
                offenders.append((top, p))
    def rel(p):
        return os.path.relpath(p, root)
    per_dir = {}   # ELF files per directory, two levels deep ("usr/lib"); a file one level down counts under its directory
    for p in elves:
        parts = rel(p).split(os.sep)
        top = "/".join(parts[:2]) if len(parts) > 2 else parts[0]
        per_dir[top] = per_dir.get(top, 0) + 1
    print("glibc-tree %s: %d ELF files (%s)" % (root, len(elves), ", ".join("%s %d" % kv for kv in sorted(per_dir.items()))))
    bad = []
    if not elves:
        bad.append("no ELF file under %s" % root)
    elif not any_glibc:
        bad.append("no ELF file under %s needs any GLIBC_ version: the audit would prove nothing" % root)
    for p in unreadable:
        bad.append("%s: an ELF file whose sections cannot be read" % rel(p))
    for top, p in sorted(offenders, key=lambda x: version_key(x[0]), reverse=True):
        bad.append("%s needs GLIBC_%s, above the limit %s" % (rel(p), top, limit))
    if worst:
        print("worst: GLIBC_%s (%s)" % (worst[0], rel(worst[1])))
    if control:
        if offenders:
            print("ok   control: the walk finds %s" % "; ".join("%s (GLIBC_%s)" % (rel(p), t) for t, p in offenders))
            return 0
        print("FAIL control: nothing above GLIBC_%s was found under %s; the gate would pass anything" % (limit, root))
        return 1
    if bad:
        for b in bad:
            print("FAIL %s" % b)
        return 1
    print("ok   every ELF file needs at most GLIBC_%s" % limit)
    return 0


# v1.1 fix (the window on stock Ubuntu). Libraries no allowlist may admit: a stock Ubuntu desktop does not install
# them (libopengl0 is not in ubuntu-desktop-minimal), and the AppImage may not bundle them either (GLVND_HOST_ONLY).
NEVER_ON_HOST = ["libOpenGL.so.0"]
# GLVND's dispatch and vendor-neutral front ends: always the host's, never in the image (they select the host's GL
# driver; a bundled copy breaks it). linuxdeploy's excludelist keeps them out; this keeps a later "fix" from bundling one.
GLVND_HOST_ONLY = ["libOpenGL.so*", "libGLdispatch.so*", "libGLX.so*", "libGL.so*", "libEGL.so*"]


def read_allowlist(path):
    globs = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                globs.append(line)
    return globs


def cmd_host_libs(root, allowlist, control=False):
    try:
        allowed = read_allowlist(allowlist)
    except OSError as e:
        print("FAIL the allowlist %s cannot be read: %s" % (allowlist, e))
        return 1
    bad = []
    if not allowed:
        bad.append("the allowlist %s is empty" % allowlist)
    for lib in NEVER_ON_HOST:
        hit = [a for a in allowed if fnmatch.fnmatchcase(lib, a)]
        if hit:
            bad.append("the allowlist admits %s (%s): a stock Ubuntu desktop does not install it" % (lib, ", ".join(hit)))
    libdir = os.path.join(root, "usr", "lib")
    entries = sorted(os.listdir(libdir)) if os.path.isdir(libdir) else []
    for name in entries:
        if any(fnmatch.fnmatchcase(name, g) for g in GLVND_HOST_ONLY):
            bad.append("usr/lib/%s is bundled: GLVND libraries must come from the host" % name)
    # A name counts as bundled only when it resolves (links followed) to a regular file INSIDE the extracted tree
    #: a dangling link, or one that escapes the image (to the build host's /usr/lib, say), is not a
    # library the image carries. Such a name is never bundled; when something NEEDs it, it is an offender by itself.
    real_root = os.path.realpath(root)
    bundled, unresolved = set(), {}
    for name in entries:
        target = os.path.realpath(os.path.join(libdir, name))
        if os.path.commonpath([real_root, target]) != real_root:
            unresolved[name] = "a link that escapes the image (to %s)" % target
        elif not os.path.isfile(target):
            unresolved[name] = ("a dangling link (to %s)" % os.path.relpath(target, real_root)
                                if not os.path.exists(target) else "not a regular file")
        else:
            bundled.add(name)
    elves, needs, offenders, host = 0, 0, {}, {}
    for d, dirs, files in os.walk(root, followlinks=False):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(d, f)
            if os.path.islink(p) or not os.path.isfile(p):
                continue
            with open(p, "rb") as fh:
                if fh.read(4) != b"\x7fELF":
                    continue
            elves += 1
            try:
                names = elf_needed(p) or []   # a static ELF file (the embedded program, say) needs nothing
            except (struct.error, IndexError, UnicodeDecodeError) as e:
                bad.append("%s: an ELF file whose dynamic section cannot be read (%s)" % (os.path.relpath(p, root), e))
                continue
            needs += len(names)
            for n in names:
                if n in bundled:
                    continue
                if n in unresolved:
                    offenders.setdefault(n, []).append(os.path.relpath(p, root))
                    continue
                if any(fnmatch.fnmatchcase(n, a) for a in allowed):
                    host.setdefault(n, []).append(os.path.relpath(p, root))
                else:
                    offenders.setdefault(n, []).append(os.path.relpath(p, root))
    print("host-libs %s: %d ELF files, %d NEEDED entries, %d bundled names in usr/lib; from the host: %s"
          % (root, elves, needs, len(bundled), " ".join(sorted(host)) or "nothing"))
    if not elves:
        bad.append("no ELF file under %s" % root)
    elif not needs:
        bad.append("no ELF file under %s needs any library: the audit would prove nothing" % root)
    for n in sorted(offenders):
        users = offenders[n]
        why = ("usr/lib/%s is %s, so it is not bundled" % (n, unresolved[n]) if n in unresolved
               else "%s is neither bundled nor a stock-desktop host library" % n)
        bad.append("%s (needed by %s%s)"
                   % (why, ", ".join(users[:3]), " and %d more" % (len(users) - 3) if len(users) > 3 else ""))
    if control:
        if offenders:
            print("ok   control: the walk refuses %s" % ", ".join(sorted(offenders)))
            return 0
        print("FAIL control: every NEEDED under %s passed; the gate would pass anything" % root)
        return 1
    if bad:
        for b in bad:
            print("FAIL %s" % b)
        return 1
    print("ok   every ELF file needs only bundled libraries or allowed host libraries (%s)" % allowlist)
    return 0


STAMP_MAGIC = b"ASB-BUILD-STAMP\0"
STAMP_SIZE = 256


def read_stamp(data):
    """The build stamp record (STAMP_SIZE bytes) of an ELF or PE image, or None with the reason."""
    if data[:4] == b"\x7fELF":
        parsed = elf_sections(data)
        if parsed is None:
            return None, "the ELF section table cannot be read"
        _is64, _end, sections, shstrndx = parsed
        if shstrndx >= len(sections):
            return None, "no section-name table"
        _n, _t, stroff, strsize, _l, _i = sections[shstrndx]
        names = data[stroff:stroff + strsize]
        for name, stype, off, size, _link, _info in sections:
            if names[name:].split(b"\0", 1)[0] == b".asb_stamp":
                if stype != 1 or size < STAMP_SIZE or off + STAMP_SIZE > len(data):
                    return None, ".asb_stamp is not a whole record inside the file"
                return data[off:off + STAMP_SIZE], None
        return None, "no .asb_stamp section"
    if data[:2] == b"MZ" and len(data) >= 0x40:
        lfanew, = struct.unpack_from("<I", data, 0x3C)
        if lfanew + 24 > len(data) or data[lfanew:lfanew + 4] != b"PE\0\0":
            return None, "not a PE image"
        count, = struct.unpack_from("<H", data, lfanew + 6)
        optional, = struct.unpack_from("<H", data, lfanew + 20)
        table = lfanew + 24 + optional
        if table + 40 * count > len(data):
            return None, "the PE section table is outside the file"
        for i in range(count):
            o = table + 40 * i
            if data[o:o + 8] == b".asbstmp":
                vsize, _va, rawsize, rawoff = struct.unpack_from("<IIII", data, o + 8)
                if vsize < STAMP_SIZE or rawsize < STAMP_SIZE or rawoff + STAMP_SIZE > len(data):
                    return None, ".asbstmp is not a whole record inside the file"
                return data[rawoff:rawoff + STAMP_SIZE], None
        return None, "no .asbstmp section"
    return None, "neither an ELF nor a PE image"


def cmd_stamp(path, version):
    record, why = read_stamp(open(path, "rb").read())
    if record is not None:
        if not record.startswith(STAMP_MAGIC):
            why = "the stamp section does not start with the magic"
        else:
            body = record[len(STAMP_MAGIC):].split(b"\0", 1)[0]
            try:
                stamp = json.loads(body.decode())
            except ValueError as e:
                stamp, why = None, "the stamp is not JSON (%s)" % e
            want = {"schema": 1, "product": "aether-serial-bridge", "version": version}
            if stamp is not None and stamp != want:
                why = "the stamp is %s, expected %s" % (stamp, want)
            elif stamp is not None:
                print("ok   %s carries the build stamp %s" % (os.path.basename(path), body.decode()))
                return 0
    print("FAIL %s: %s" % (path, why))
    return 1


def cmd_gui_report(path, status):
    # The exit status first: the program prints its report before it tears the window down, so a crash or a hang in
    # teardown still leaves a good report behind. timeout(1) gives 124 when the time ran out, 128+N for signal N.
    try:
        status = int(status)
    except ValueError:
        print("FAIL %s: the self-test's exit status %r is not a number" % (path, status))
        return 1
    if status != 0:
        why = ("timed out" if status == 124 else "killed by signal %d" % (status - 128) if status > 128
               else "exited %d" % status)
        print("FAIL %s: the window self-test %s (it must exit 0)" % (path, why))
        cmd_gui_report_body(path)
        return 1
    return cmd_gui_report_body(path)


def cmd_gui_report_body(path):
    try:
        lines = [l for l in open(path, encoding="utf-8", errors="replace").read().splitlines() if l.strip().startswith("{")]
        report = json.loads(lines[-1])
    except (OSError, ValueError, IndexError) as e:
        print("FAIL %s: no self-test report (%s)" % (path, e))
        return 1
    ok = report.get("exposed") is True and report.get("painted") is True
    print("%s window self-test: exposed=%s painted=%s, %d modules listed"
          % ("ok  " if ok else "FAIL", report.get("exposed"), report.get("painted"), len(report.get("modules") or [])))
    return 0 if ok else 1


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd, args = argv[1], argv[2:]
    if cmd == "seams" and args:
        return cmd_seams(args)
    if cmd == "seams-control" and args:
        return cmd_seams(args, control=True)
    if cmd in ("digest", "digest-control") and len(args) in (2, 3):
        # A Linux window (ELF) or the AppImage's AppRun (a script) is run; anything else (the Windows GUI) is searched.
        try:
            with open(args[0], "rb") as f:
                runnable = f.read(4) in (b"\x7fELF",) or os.path.basename(args[0]) == "AppRun"
        except OSError:
            runnable = True   # reported by the run
        if runnable:
            return cmd_digest_elf(*args, control=cmd == "digest-control")
        if cmd == "digest" and len(args) == 2:
            return cmd_digest(*args)
    if cmd == "glibc-tree" and len(args) == 2:
        return cmd_glibc_tree(*args)
    if cmd == "glibc-tree-control" and len(args) == 2:
        return cmd_glibc_tree(*args, control=True)
    if cmd == "host-libs" and len(args) == 2:
        return cmd_host_libs(*args)
    if cmd == "host-libs-control" and len(args) == 2:
        return cmd_host_libs(*args, control=True)
    if cmd == "stamp" and len(args) == 2:
        return cmd_stamp(*args)
    if cmd == "zip" and len(args) >= 2:
        return cmd_zip(args[0], args[1:])
    if cmd == "same" and len(args) == 2:
        return cmd_same(*args)
    if cmd == "needed" and len(args) >= 2:
        return cmd_needed(args[0], args[1:])
    if cmd == "needed-control" and len(args) >= 2:
        return cmd_needed(args[0], args[1:], control=True)
    if cmd == "gui-report" and len(args) == 2:
        return cmd_gui_report(*args)
    if cmd == "sha256sums" and len(args) == 2:
        return cmd_sha256sums(*args)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
