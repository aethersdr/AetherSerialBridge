#!/usr/bin/env bash
# The stock Ubuntu desktop image for tools/stock-desktop-start.sh (v1.1 fix, the window on stock Ubuntu): built once
# per machine per version of ci/containers/stock-desktop-ubuntu2604.Dockerfile (a hosted runner starts empty, so in
# CI that is once per release job).
#
# usage: ci/scripts/stock-desktop-image.sh     prints the image reference (asb-stock-desktop-2604:<tag>)
#
# Unlike asb-build-2204 and the systemd images (rebuilt every leg with --pull, Docker's layer cache keeping it quick),
# this image takes 1,275 packages and over an hour cold, so it is never rebuilt behind our back: the base is pinned by
# digest in the Dockerfile, there is no --pull, and the tag is the first 16 hex digits of the Dockerfile's SHA-256. A
# changed Dockerfile is a new tag (built once); an unchanged one is the cached image, whatever the archive or Docker
# Hub did since. Older tags of this repository are removed after a successful build: they are not dangling, so a
# machine that keeps images would otherwise keep every 2.7 GB version forever.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ctx="$here/../containers"
file="$ctx/stock-desktop-ubuntu2604.Dockerfile"
repo=asb-stock-desktop-2604
tag="$(sha256sum "$file" | cut -c1-16)"
ref="$repo:$tag"
if docker image inspect "$ref" >/dev/null 2>&1; then
    echo "cached: $ref" >&2
else
    echo "building $ref (no cached image for this Dockerfile; over an hour cold)" >&2
    docker build --label org.aethersdr.asb-ci=containers --label "org.aethersdr.asb-stock-desktop.dockerfile-sha256=$tag" \
        -t "$ref" -f "$file" "$ctx" >&2
    docker images "$repo" --format '{{.Tag}}' | { grep -vx "$tag" || true; } | while read -r old; do
        echo "removing the older $repo:$old" >&2
        docker rmi "$repo:$old" >/dev/null || echo "::warning::could not remove $repo:$old" >&2
    done
fi
echo "$ref"
