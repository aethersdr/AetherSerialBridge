#!/usr/bin/env python3
"""Static check (v1.1 WS-19): the operator docs' relative links and images resolve, so every page renders on GitHub.

Checks README.md, CHANGELOG.md, docs/*.md and docs/images/README.md: every relative Markdown link or image (`[..](path)`,
`![..](path)`) and every HTML `<img src="path">` names a file that exists, and a `#fragment` on a link to a Markdown
file (or to the page itself) names one of its headings, slugged as GitHub does. External links (a scheme, or `//`)
are not fetched.

Usage: check_doc_links.py [--root <repository root>]   (exit 0 clean, 1 findings)
"""
import argparse
import re
import sys
from pathlib import Path

LINK = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
IMG = re.compile(r"<img\s[^>]*src=\"([^\"]+)\"")
HEADING = re.compile(r"^(#{1,6})\s+(.+?)\s*#*\s*$")
FENCE = re.compile(r"^\s*(```|~~~)")


def slug(text):
    t = re.sub(r"`([^`]*)`", r"\1", text).strip().lower()
    t = re.sub(r"[^\w\- ]", "", t)
    return t.replace(" ", "-")


def anchors(path):
    out, seen, fenced = set(), {}, False
    for line in path.read_text(encoding="utf-8").splitlines():
        if FENCE.match(line):
            fenced = not fenced
            continue
        m = None if fenced else HEADING.match(line)
        if m:
            s = slug(m.group(2))
            n = seen.get(s, 0)
            seen[s] = n + 1
            out.add(s if n == 0 else f"{s}-{n}")
    return out


def targets(text):
    fenced = False
    for line in text.splitlines():
        if FENCE.match(line):
            fenced = not fenced
            continue
        if fenced:
            continue
        line = re.sub(r"`[^`]*`", "", line)
        for rx in (LINK, IMG):
            for m in rx.finditer(line):
                yield m.group(1)


def check(root):
    root = Path(root)
    pages = [root / "README.md", *sorted((root / "docs").glob("*.md")), root / "docs" / "images" / "README.md"]
    if (root / "CHANGELOG.md").is_file():
        pages.insert(1, root / "CHANGELOG.md")
    findings = []
    for page in pages:
        if not page.exists():
            continue
        for target in targets(page.read_text(encoding="utf-8")):
            if re.match(r"^[a-z][a-z0-9+.-]*:", target) or target.startswith("//"):
                continue
            path, _, frag = target.partition("#")
            dest = (page.parent / path).resolve() if path else page
            rel = page.relative_to(root)
            if not dest.exists():
                findings.append(f"{rel}: {target}: no such file")
                continue
            if frag and dest.suffix == ".md" and frag not in anchors(dest):
                findings.append(f"{rel}: {target}: no heading #{frag} in {dest.relative_to(root)}")
    return findings


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    findings = check(ap.parse_args(argv).root)
    if findings:
        print("check_doc_links: FAILED")
        for f in findings:
            print("  -", f)
        return 1
    print("check_doc_links: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
