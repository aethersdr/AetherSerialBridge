#!/usr/bin/env python3
"""Create a DRAFT GitHub release for a tag and upload the release artifacts to it (WS-13, release.yml's publish job).

usage: packaging/publish-release.py <owner/repo> <tag> <artifact dir>      (token in GITHUB_TOKEN)

Only release.yml's publish job runs this, and only on a tag push. The release is a draft: a maintainer reads it,
checks the artifacts against SHA256SUMS and docs/release-checklist.md, and publishes it in the GitHub UI. Any release
that already exists for the tag, published OR draft, is refused: the script pages through the repository's releases
(GET /releases/tags/{tag} only sees published ones), so a re-run after a failed upload never makes a second draft
with a different file set. Delete the stale draft by hand first.
Standard library only."""
import json
import mimetypes
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

API = "https://api.github.com"


def call(method, url, token, body=None, ctype="application/json"):
    data = json.dumps(body).encode() if isinstance(body, dict) else body
    req = urllib.request.Request(url, data=data, method=method, headers={
        "Authorization": "Bearer %s" % token, "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28", "Content-Type": ctype})
    with urllib.request.urlopen(req, timeout=600) as r:
        return json.loads(r.read() or b"{}")


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    repo, tag, folder = argv[1:]
    token = os.environ.get("GITHUB_TOKEN")
    if not token:
        print("GITHUB_TOKEN is not set")
        return 2
    files = sorted(f for f in os.listdir(folder) if os.path.isfile(os.path.join(folder, f)))
    if "SHA256SUMS" not in files:
        print("refusing: no SHA256SUMS in %s" % folder)
        return 1
    page = 1
    while True:
        batch = call("GET", "%s/repos/%s/releases?per_page=100&page=%d" % (API, repo, page), token)
        for r in batch:
            if r.get("tag_name") == tag:
                print("refusing: a %s release for %s already exists: %s"
                      % ("draft" if r.get("draft") else "published", tag, r.get("html_url")))
                return 1
        if len(batch) < 100:
            break
        page += 1
    rel = call("POST", "%s/repos/%s/releases" % (API, repo), token, {
        "tag_name": tag, "name": "Aether Serial Bridge %s" % tag, "draft": True, "prerelease": False,
        "body": "Draft built by release.yml from %s. Verify the files with SHA256SUMS and complete "
                "docs/release-checklist.md before publishing. v1 is unsigned." % tag})
    upload = rel["upload_url"].split("{")[0]
    for name in files:
        path = os.path.join(folder, name)
        ctype = mimetypes.guess_type(name)[0] or "application/octet-stream"
        with open(path, "rb") as f:
            call("POST", "%s?name=%s" % (upload, urllib.parse.quote(name)), token, f.read(), ctype)
        print("uploaded %s (%d bytes)" % (name, os.path.getsize(path)))
    print("draft release %s: %s" % (tag, rel.get("html_url")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
