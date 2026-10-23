# Black-box systemd environment for the Linux service tests (master 8.1): debian:bookworm
# (glibc 2.36, newer than the 2.35 build floor) with systemd as PID 1, dbus and polkit.
#
# WS-1 ships the skeleton: the image, and ci/scripts/systemd-smoke.sh, which runs the BUILT
# headless binary's --help and --status inside it. WS-8 adds the install/apply/reboot cases on the
# same image.
#
# The binary and its Qt come in by bind mount, never baked into the image:
#   /usr/local/bin/aether-serial-bridge   the built headless executable (read-only)
#   /qt/<QT_VERSION>/gcc_64               the Qt tree it was built against, at the path the build
#                                         container configured it at, so the binary's RUNPATH
#                                         resolves with no LD_LIBRARY_PATH (read-only)
FROM debian:bookworm

ENV DEBIAN_FRONTEND=noninteractive

# libdbus-1-3: QtDBus loads it at run time. libglib2.0-0, libicu: not needed by the pinned Qt
# (aqt ships its own ICU); present so a missing system library fails the smoke here, loudly, not in
# the field. libbrotli1, libgssapi-krb5-2: the dynamic dev build's QtNetwork needs them once the
# headless executable runs the engine (WS-8 --service); the release binaries link Qt statically (D3).
# util-linux (unshare, runuser), inotify-tools: the WS-8 systemd suite's observations.
# fuse3 (and NOT libfuse2): appimage_starts_without_libfuse2 (v1.1 D68) mounts the AppImage with fusermount3 only.
# The graphics and font libraries every desktop has, which the AppImage leaves to the system (linuxdeploy's
# excludelist): the window's self-test runs from the mounted image in that case.
RUN apt-get update && apt-get install -y --no-install-recommends \
        systemd \
        fuse3 \
        libgl1 libegl1 libopengl0 libglx0 libx11-6 libx11-xcb1 libxcb1 libfontconfig1 libfreetype6 \
        libxkbcommon0 \
        libbrotli1 \
        libgssapi-krb5-2 \
        inotify-tools \
        systemd-sysv \
        dbus \
        dbus-user-session \
        libdbus-1-3 \
        policykit-1 \
        socat \
        strace \
        python3 \
        procps \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    # Units a container has no use for, so boot is fast and quiet.
    && systemctl mask \
        systemd-logind.service \
        getty.target \
        console-getty.service \
        systemd-udevd.service \
        systemd-udev-trigger.service \
        systemd-timesyncd.service

# v1.1 WS-15: the polkit harness's users. tuser (uid 1000) runs the window's side; tadmin is the administrator polkit's
# auth_admin asks for (Debian's admin rule: unix-group:sudo), with a fixed TEST password that pkttyagent answers on a
# pseudo-terminal (bootstrap_auth_admin_pkttyagent). Nothing else changes.
RUN useradd -m -u 1000 tuser \
    && useradd -m -G sudo tadmin \
    && echo 'tadmin:asb-test-admin' | chpasswd

STOPSIGNAL SIGRTMIN+3
CMD ["/lib/systemd/systemd"]
