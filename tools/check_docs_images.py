#!/usr/bin/env python3
"""Static check (INV-135, v1.1 WS-19): the documentation images carry nothing real.

Every image under docs/images/ is rendered from the demo fixture (docs/images/demo/) by tools/docs_shots.py, which
writes beside each PNG a manifest (<name>.json) listing every text the image shows. This check fails when:

  * a PNG outside a manual/ folder has no manifest, or a manifest names another image or has no texts;
  * a manifest string (any field: the state, the theme, the texts) holds an IP address outside the documentation
    ranges (192.0.2.0/24, 198.51.100.0/24, 203.0.113.0/24, 2001:db8::/32), loopback and the wildcards;
  * a manifest string holds a word shaped like an amateur callsign (CALLSIGN below, in either case), unless the
    fixture's allow-list (docs/images/demo/allow-list.json, "words") names it;
  * a manifest string holds an absolute path that is not one of the allow-list's "paths" or inside one (whole
    components; a path cut short with "\u2026" keeps at least one), or a host name that is not one of its "hosts";
  * any PNG (manual/ ones included: they are captured by hand) carries text metadata (tEXt, zTXt, iTXt, eXIf), so a
    capture tool's user name, host or path cannot ride along;
  * docs/images/ is 4 MB or more in total.

The fixture files themselves are held to the same address, callsign, path and host rules, so the allow-list can only
ever exempt the fixture's own words.

Hand-captured images (docs/images/<os>/manual/) have no manifest: a person checks them against the checklist in
docs/images/README.md.

Usage: check_docs_images.py [--root <repository root>]   (exit 0 clean, 1 findings)
"""
import argparse
import ipaddress
import json
import re
import struct
import sys
from pathlib import Path

# A callsign: a prefix of one or two letters, or a digit and one or two letters (2E0, 9A1, 3DA0), a digit, then one
# to four letters; either case, since a hand-typed port name or device label may be lower case.
CALLSIGN = re.compile(r"(?i)\b(?:[A-Z]{1,2}|[0-9][A-Z]{1,2})[0-9][A-Z]{1,4}\b")
DOC_NETS = [ipaddress.ip_network(n) for n in
            ("192.0.2.0/24", "198.51.100.0/24", "203.0.113.0/24", "2001:db8::/32", "127.0.0.0/8", "::1/128")]
WILDCARDS = {ipaddress.ip_address("0.0.0.0"), ipaddress.ip_address("::")}
# IPv4: four dotted groups not inside a longer dotted number (a version such as 1.2.3.4.5). A full stop that ends
# the sentence ("Could not bind 10.0.0.5.") is not part of a number, so only a full stop before a digit blocks.
IPV4 = re.compile(r"(?<![\w])(?<!\d\.)(\d{1,3}(?:\.\d{1,3}){3})(?:/\d{1,2})?(?::\d+)?(?![\w])(?!\.\d)")
# IPv6: the whole token of hex digits, colons and dots (all eight groups, a leading or trailing "::", an IPv4 tail),
# not inside a word, taken before it is parsed; a sentence's full stop after it is dropped. A token with at least two
# colons is a candidate: it must parse to an allowed address, or, when it looks like an address (a "::", a hex letter,
# or seven colons), it is reported as one that does not parse. A time such as 12:00:05 looks like neither.
IPV6 = re.compile(r"(?<![\w:.])([0-9A-Fa-f:.]*:[0-9A-Fa-f:.]*:[0-9A-Fa-f:.]*)(?![\w:])")
# An absolute path: a POSIX path (a slash after a space, a quote, a bracket or the start) or a Windows drive path.
POSIX_PATH = re.compile(r"(?:(?<=^)|(?<=[\s\"'(\[=]))(/[^\s\"'()\[\],;]+)")
WIN_PATH = re.compile(r"\b([A-Za-z]:[\\/][^\"'()\[\],;]*)")
# A host name: dotted labels ending in a top-level domain that is a word, not a file extension.
HOST = re.compile(r"\b((?:[A-Za-z0-9-]+\.)+([A-Za-z]{2,24}))\b")
FILE_EXTENSIONS = {"json", "log", "exe", "png", "txt", "md", "service", "policy", "conf", "lock", "id", "token",
                   "pending", "dll", "sh", "AppImage", "zip", "cpp", "h", "ps1", "py", "yml", "ini", "msc", "cpl"}
METADATA_CHUNKS = {b"tEXt", b"zTXt", b"iTXt", b"eXIf"}
SIZE_BUDGET = 4 * 1024 * 1024


def load_allow(root):
    p = root / "docs" / "images" / "demo" / "allow-list.json"
    data = json.loads(p.read_text(encoding="utf-8"))
    return {
        "words": set(data.get("words", [])),
        "paths": list(data.get("paths", [])),
        "hosts": set(data.get("hosts", [])),
    }


def address_findings(text):
    out = []
    for m in IPV4.finditer(text):
        try:
            a = ipaddress.ip_address(m.group(1))
        except ValueError:
            continue
        if a in WILDCARDS or any(a in n for n in DOC_NETS):
            continue
        out.append(f"address {m.group(0)} is outside the documentation ranges")
    for m in IPV6.finditer(text):
        cand = m.group(1).rstrip(".")
        if cand.count(":") < 2:
            continue
        try:
            a = ipaddress.IPv6Address(cand)
        except ValueError:
            if "::" in cand or re.search(r"[A-Fa-f]", cand) or cand.count(":") >= 7:
                out.append(f"address {cand} does not parse (an address-like token)")
            continue
        mapped = a.ipv4_mapped
        if mapped is not None and any(mapped in n for n in DOC_NETS if n.version == 4):
            continue
        if a in WILDCARDS or any(a.version == n.version and a in n for n in DOC_NETS):
            continue
        out.append(f"address {cand} is outside the documentation ranges")
    return out


def path_allowed(path, allowed):
    """An allowed path, or a path inside one, matched on whole components (/usr/local/bin allows /usr/local/bin/x, not
    /usr/local/binary). A path the screen cut short ("/dev/serial/b\u2026") must keep at least one whole component and
    be the start of an allowed path, or lie inside one."""
    norm = path.replace("\\", "/").lower()
    prefixes = [p.replace("\\", "/").lower().rstrip("/") for p in allowed]

    def inside(n, p):
        return n == p or n.startswith(p + "/")

    if norm.endswith("\u2026"):
        cut = norm[:-1]
        head = cut[:cut.rfind("/") + 1]
        if not [c for c in head.split("/") if c and not c.endswith(":")]:
            return False
        return any(p.startswith(cut) or inside(cut, p) for p in prefixes)
    return any(inside(norm, p) for p in prefixes)


def word_findings(text, allow):
    out = []
    for m in CALLSIGN.finditer(text):
        if m.group(0) not in allow["words"]:
            out.append(f"callsign-shaped word {m.group(0)!r}")
    for rx in (POSIX_PATH, WIN_PATH):
        for m in rx.finditer(text):
            path = m.group(1).rstrip(".:")
            if len(path) < 2:
                continue
            if not path_allowed(path, allow["paths"]):
                out.append(f"path {path!r} is not on the allow-list")
    for m in HOST.finditer(text):
        name, tld = m.group(1), m.group(2)
        if tld in FILE_EXTENSIONS or tld.lower() in {e.lower() for e in FILE_EXTENSIONS}:
            continue
        if re.fullmatch(r"[\d.]+", name) or name in allow["hosts"]:
            continue
        out.append(f"host name {name!r} is not on the allow-list")
    return out


def text_findings(text, allow):
    return address_findings(text) + word_findings(text, allow)


def png_chunks(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    i, chunks = 8, []
    while i + 8 <= len(data):
        length, ctype = struct.unpack(">I4s", data[i:i + 8])
        chunks.append(ctype)
        i += 12 + length
        if ctype == b"IEND":
            break
    return chunks


def strings_in(obj):
    if isinstance(obj, str):
        yield obj
    elif isinstance(obj, list):
        for v in obj:
            yield from strings_in(v)
    elif isinstance(obj, dict):
        for k, v in obj.items():
            yield k
            yield from strings_in(v)


def manifest_findings(manifest, allow):
    """Every string in a manifest (state, theme, texts and any other field, keys included), not only its texts."""
    out = []
    for s in strings_in(manifest):
        out += text_findings(s, allow)
    return out


def check(root):
    root = Path(root)
    images = root / "docs" / "images"
    findings = []
    if not images.is_dir():
        return [f"{images}: missing"]
    allow = load_allow(root)

    # The fixture is held to the same rules: the allow-list can exempt only what the fixture itself says.
    for f in sorted((images / "demo").glob("*.json")):
        if f.name == "allow-list.json":
            continue
        for s in strings_in(json.loads(f.read_text(encoding="utf-8"))):
            findings += [f"{f.relative_to(root)}: {x}" for x in text_findings(s, allow)]

    total = 0
    for p in sorted(images.rglob("*")):
        if p.is_file():
            total += p.stat().st_size
    if total >= SIZE_BUDGET:
        findings.append(f"docs/images is {total} bytes; the budget is under {SIZE_BUDGET}")

    for png in sorted(images.rglob("*.png")):
        rel = png.relative_to(root)
        try:
            bad = METADATA_CHUNKS.intersection(png_chunks(png.read_bytes()))
        except ValueError as e:
            findings.append(f"{rel}: {e}")
            continue
        if bad:
            findings.append(f"{rel}: carries metadata chunks {sorted(c.decode() for c in bad)} (re-save with Pillow)")
        if "manual" in png.relative_to(images).parts:
            continue
        manifest = png.with_suffix(".json")
        if not manifest.exists():
            findings.append(f"{rel}: no manifest ({manifest.name})")
            continue
        try:
            m = json.loads(manifest.read_text(encoding="utf-8"))
        except json.JSONDecodeError as e:
            findings.append(f"{manifest.relative_to(root)}: not JSON ({e})")
            continue
        if m.get("image") != png.name:
            findings.append(f"{manifest.relative_to(root)}: names image {m.get('image')!r}, not {png.name!r}")
        texts = m.get("texts")
        if not isinstance(texts, list) or not texts or not all(isinstance(t, str) for t in texts):
            findings.append(f"{manifest.relative_to(root)}: no texts")
            continue
        findings += [f"{manifest.relative_to(root)}: {x}" for x in manifest_findings(m, allow)]

    for manifest in sorted(images.rglob("*.json")):
        if manifest.parent.name == "demo" or "manual" in manifest.relative_to(images).parts:
            continue
        if not manifest.with_suffix(".png").exists():
            findings.append(f"{manifest.relative_to(root)}: a manifest with no image")
    return findings


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    args = ap.parse_args(argv)
    findings = check(args.root)
    if findings:
        print("check_docs_images: FAILED")
        for f in findings:
            print("  -", f)
        return 1
    print("check_docs_images: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
