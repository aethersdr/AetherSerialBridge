# Build environment for release Linux binaries (D36): ubuntu:22.04, so the glibc floor is 2.35.
#
# This image holds the toolchain and the system libraries Qt links against, with the X11/xcb development headers a
# from-source GUI Qt needs (the arm64 window's shared Qt, packaging/qt-static/profiles/linux-gui-shared.txt: aqt's
# linux_arm64 Qt needs GLIBC_2.38, above the 2.35 floor). It deliberately holds
# NO Qt: Qt comes from aqtinstall into a directory the caller bind-mounts at /qt
# (ci/scripts/install-qt.sh), so the same tree is cached between runs and is the same tree on a
# developer machine and in CI.
#
# Run it as the caller's user, with bind mounts only:
#   docker run --rm -u "$(id -u):$(id -g)" -e HOME=/work/home \
#     -v "$PWD:/src" -v "$PWD/build:/build" -v "$QT_CACHE:/qt" <image> <command>
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ninja-build \
        pkg-config \
        git \
        curl \
        ca-certificates \
        python3 \
        python3-venv \
        python3-pip \
        binutils \
        file \
        strace \
        libdbus-1-dev \
        libgl1-mesa-dev \
        libxkbcommon-dev \
        libfontconfig1-dev \
        libfreetype-dev \
        libx11-xcb-dev \
        libxcb1-dev \
        libxcb-cursor0 \
        libxcb-glx0 \
        libxcb-icccm4 \
        libxcb-image0 \
        libxcb-keysyms1 \
        libxcb-randr0 \
        libxcb-render-util0 \
        libxcb-shape0 \
        libxcb-sync1 \
        libxcb-util1 \
        libxcb-xfixes0 \
        libxcb-xinerama0 \
        libxcb-xkb1 \
        libxkbcommon-x11-0 \
        libxkbcommon-x11-dev \
        libx11-dev \
        libxcb-cursor-dev \
        libxcb-icccm4-dev \
        libxcb-image0-dev \
        libxcb-keysyms1-dev \
        libxcb-randr0-dev \
        libxcb-render-util0-dev \
        libxcb-shape0-dev \
        libxcb-shm0-dev \
        libxcb-sync-dev \
        libxcb-util-dev \
        libxcb-xfixes0-dev \
        libxcb-xinerama0-dev \
        libxcb-xkb-dev \
    && rm -rf /var/lib/apt/lists/*

# cmake: Ubuntu 22.04 ships 3.22, which is below what Qt 6.12's CMake package may require
# (checked by the first configure). A pinned release from PyPI, in a venv outside the Qt tree.
ENV CMAKE_VERSION=3.31.6
RUN python3 -m venv /usr/local/lib/cmake-venv \
    && /usr/local/lib/cmake-venv/bin/pip install --no-cache-dir "cmake==${CMAKE_VERSION}" \
    && ln -s /usr/local/lib/cmake-venv/bin/cmake /usr/local/bin/cmake \
    && ln -s /usr/local/lib/cmake-venv/bin/ctest /usr/local/bin/ctest \
    && cmake --version

WORKDIR /src
