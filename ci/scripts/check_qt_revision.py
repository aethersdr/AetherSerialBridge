#!/usr/bin/env python3
"""Prove which Qt package revision aqtinstall installed, and keep the proof.

Qt republishes the same version string under a new package revision (the <Version> suffix in the
online repository's Updates.xml, e.g. 6.12.0-0-202609280346). aqt installs whatever the repository
currently serves for the version and does not take a revision, so a same-version republish would
install different packages under an unchanged pin. What aqt does leave behind is its log: every
archive it downloads is logged as

    Download URL: https://.../qt.qt6.6120.linux_gcc_64/6.12.0-0-202609280346qtbase-Linux-....7z

and the revision is part of that URL, so it is the revision of the archive that was fetched (aqt
checks each archive against the repository's published SHA-256 before it extracts it). This script
reads those lines and fails unless every archive carries the pinned version and revision.

    check_qt_revision.py install  --log aqtinstall.log --version 6.12.0 --revision R --out EVIDENCE
        exit 0 and write EVIDENCE only if the log proves it; exit 1 with the reason otherwise.
    check_qt_revision.py evidence --file EVIDENCE --version 6.12.0 --revision R
        exit 0 only if EVIDENCE was written by a successful `install` for this version/revision.
        This is the check on a cache hit: a tree restored from a cache must have been proven.

A log with no download lines, a line that does not parse, or a missing qtbase / qtserialport
archive is a failure, never a pass: if aqt stops logging this way the check must go red, not quiet.
Standard library only; runs on the Linux and Windows runners alike.
"""
import argparse
import re
import sys
from pathlib import Path

URL_LINE = re.compile(r"Download URL:\s*(\S+)")
# .../<package>/<qt version>-<build>-<revision><archive name>.7z   (no separator before the name)
ARCHIVE = re.compile(
    r"/(?P<package>[^/]+)/(?P<qt>\d+(?:\.\d+)+)-(?P<build>\d+)-(?P<revision>\d{12})(?!\d)"
    r"(?P<archive>[^/]+\.7z)$")
REQUIRED = ("qtbase-", "qtserialport-")   # the archives the build asks for (install-qt.sh -m qtserialport)
HEADER = "qt {version} package-revision {revision}"


def prove(log_text, version, revision):
    """Return (evidence lines, problems) for an aqt log."""
    urls = URL_LINE.findall(log_text)
    problems = []
    if not urls:
        return [], ["the aqt log has no 'Download URL:' lines (no archive was downloaded, or aqt "
                    "no longer logs them)"]
    seen = []
    for url in urls:
        m = ARCHIVE.search(url)
        if not m:
            problems.append(f"unrecognised archive URL: {url}")
            continue
        if m["qt"] != version:
            problems.append(f"{m['package']}/{m['archive']}: Qt {m['qt']}, pinned {version}")
        elif m["revision"] != revision:
            problems.append(f"{m['package']}/{m['archive']}: package revision {m['revision']}, "
                            f"pinned {revision}")
        seen.append(f"{m['package']}/{m['archive']}")
    for need in REQUIRED:
        if not any(f"/{need}" in s for s in seen):
            problems.append(f"no {need}* archive was downloaded")
    return seen, problems


def cmd_install(args):
    try:
        text = Path(args.log).read_text(errors="replace")
    except OSError as e:
        print(f"check_qt_revision: cannot read the aqt log: {e}", file=sys.stderr)
        return 1
    seen, problems = prove(text, args.version, args.revision)
    if problems:
        print(f"check_qt_revision: FAILED: the installed Qt is not package revision {args.revision}:",
              file=sys.stderr)
        for p in problems[:5]:
            print(f"  - {p}", file=sys.stderr)
        if len(problems) > 5:
            print(f"  ... and {len(problems) - 5} more", file=sys.stderr)
        print("  The pin (cmake/qt-pin.env QT_PACKAGE_REVISION) is stale, or Qt republished "
              f"{args.version}.", file=sys.stderr)
        return 1
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(HEADER.format(version=args.version, revision=args.revision) + "\n"
                   + "\n".join(seen) + "\n")
    print(f"check_qt_revision: ok: {len(seen)} archives, Qt {args.version} package revision "
          f"{args.revision}")
    return 0


def cmd_evidence(args):
    try:
        lines = Path(args.file).read_text().splitlines()
    except OSError as e:
        print(f"check_qt_revision: no install evidence: {e}", file=sys.stderr)
        return 1
    want = HEADER.format(version=args.version, revision=args.revision)
    body = lines[1:] if lines else []
    if not lines or lines[0] != want:
        print(f"check_qt_revision: {args.file} does not prove '{want}' (first line: "
              f"{lines[0] if lines else '<empty>'!r}); this Qt was installed without a revision check",
              file=sys.stderr)
        return 1
    for need in REQUIRED:
        if not any(f"/{need}" in s for s in body):
            print(f"check_qt_revision: {args.file} lists no {need}* archive", file=sys.stderr)
            return 1
    print(f"check_qt_revision: evidence ok: Qt {args.version} package revision {args.revision}")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    ins = sub.add_parser("install")
    ins.add_argument("--log", required=True)
    ins.add_argument("--out", required=True)
    ev = sub.add_parser("evidence")
    ev.add_argument("--file", required=True)
    for p in (ins, ev):
        p.add_argument("--version", required=True)
        p.add_argument("--revision", required=True)
    args = ap.parse_args(argv)
    return cmd_install(args) if args.cmd == "install" else cmd_evidence(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
