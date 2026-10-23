# A STOCK Ubuntu desktop userland (v1.1 fix, the window on stock Ubuntu): what a "minimal installation" of Ubuntu 26.04
# Desktop puts on a machine, and nothing of ours. release.yml starts the extracted AppImage's window in it
# (`--self-test-init`, offscreen), so a host library the image needs but a stock desktop lacks fails the release here
# instead of on a user's machine (libOpenGL.so.0, found 2026-10-19 on a stock Ubuntu 26.04.1 VM: the build image
# asb-build-2204 has every GL library, so the window started there and nowhere else).
#
# NEVER add a package to make the window start: that is the bug this image exists to find. The image only differs
# from a stock install by what a container cannot hold (no kernel, no display, no FUSE: the AppImage runs extracted)
# and by the unprivileged user the window runs as.
#
# The base is pinned by the multi-arch INDEX digest of ubuntu:26.04 (amd64 and arm64), as of
# 2026-10-20 (the image published 2026-10-06), so a Docker Hub refresh never starts a 1,275-package rebuild on a tag.
# To bump it (a deliberate change, say for a 26.04 point release): `docker buildx imagetools inspect ubuntu:26.04`,
# take the top-level "Digest:" (the index, not a per-platform manifest), replace it here and update the date. Any
# change to this file changes the image's tag (ci/scripts/stock-desktop-image.sh tags it with this file's SHA-256),
# so a machine rebuilds once and never reuses an image built from an older file.
FROM ubuntu:26.04@sha256:c098726dd9b9733f3e885f8de0ef375206a311b0a317eadc40d347b1115fb944

ENV DEBIAN_FRONTEND=noninteractive

# 1,275 packages (780 MB of archives, 2.7 GB installed): the first build took 88 minutes on a build host, so a machine
# that keeps images builds it once per version of this file (ci/scripts/stock-desktop-image.sh); a hosted runner builds
# it in every release job. apt still installs what the archive serves at build time: the userland drifts only when this
# file changes, and tools/stock-desktop-start.sh checks the GL facts it relies on every run. dpkg's force-unsafe-io only skips fsync while unpacking (a layer is
# written once): it changes nothing that is installed.
# ubuntu-desktop-minimal WITH its recommends (apt's default, as the installer's minimal installation): the GL stack
# (libgl1, libegl1, libglx0 from glvnd), X11/xcb, fontconfig and freetype arrive the way they arrive on a desktop.
RUN echo force-unsafe-io > /etc/dpkg/dpkg.cfg.d/asb-unsafe-io \
    && apt-get update && apt-get install -y ubuntu-desktop-minimal \
    && rm -rf /var/lib/apt/lists/* \
    && (useradd -m -s /bin/bash desktop || true)

USER desktop
WORKDIR /home/desktop
