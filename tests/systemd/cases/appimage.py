"""The AppImage's --install path (WS-13): the Linux window's image embeds the release headless binary, and
`sudo ./aether-serial-bridge-gui-<x86_64|aarch64>.AppImage --install` installs THAT binary. Run in extract mode (no FUSE in the
container, as on many hosts: APPIMAGE_EXTRACT_AND_RUN=1). Needs driver.py --dist (the release artifacts)."""

import json

from sysd import CONFIG_A, DATA, DIST, INSTALLED, UNIT, CaseSkip, case, expect

# The release file names for this machine (the x86_64 and the aarch64 AppImage each embed their own release binary).
NAMES = {"x86_64": ("aether-serial-bridge-gui-x86_64.AppImage", "aether-serial-bridge-linux-x64"),
         "aarch64": ("aether-serial-bridge-gui-aarch64.AppImage", "aether-serial-bridge-linux-arm64")}

@case("appimage_install_extract_mode")
def appimage_install(box):
    machine = box.sh("uname -m").strip()
    if machine not in NAMES:
        raise CaseSkip("no AppImage is built for %s" % machine)
    IMAGE, LINUX_BINARY = (DIST + "/" + n for n in NAMES[machine])
    if not box.exists(IMAGE):
        raise CaseSkip("no release AppImage mounted (driver.py --dist)")
    # Docker's /tmp is a noexec tmpfs: the runtime extracts under TMPDIR.
    r = box.run("mkdir -p /var/tmp/appimage && cd /var/tmp/appimage && "
                "TMPDIR=/var/tmp/appimage APPIMAGE_EXTRACT_AND_RUN=1 %s --install" % IMAGE, timeout=300)
    expect(r.rc == 0, "the AppImage's --install failed: %r" % r)
    expect(box.active() == "active", "the service does not run after the AppImage's --install")
    installed = box.sh("sha256sum %s" % INSTALLED).split()[0]
    shipped = box.sh("sha256sum %s" % LINUX_BINARY).split()[0]
    expect(installed == shipped, "the AppImage installed %s, not %s (%s)" % (installed, LINUX_BINARY, shipped))
    r = box.run("%s --status" % INSTALLED)
    expect(r.rc == 0 and "Service: running" in r.out, "--status of the installed program: %r" % r)


@case("appimage_starts_without_libfuse2")
def appimage_starts_without_libfuse2(box):
    """v1.1 D68: the AppImage (not extracted) starts with only fuse3 on the machine: the pinned type-2 runtime mounts
    itself with fusermount3, so no libfuse2 is needed. As a user (uid 1000), APPIMAGE_EXTRACT_AND_RUN unset, the
    window's --self-test-init runs offscreen from the mounted image and reports exposed and painted, with its modules
    loaded from the image's .mount_ directory. SKIP only when this container cannot mount FUSE at all (no /dev/fuse
    or no fusermount3: then the check is hand test L-1), never because the AppImage failed."""
    machine = box.sh("uname -m").strip()
    if machine not in NAMES:
        raise CaseSkip("no AppImage is built for %s" % machine)
    image = DIST + "/" + NAMES[machine][0]
    if not box.exists(image):
        raise CaseSkip("no release AppImage mounted (driver.py --dist)")
    if box.run("test -c /dev/fuse").rc != 0:
        raise CaseSkip("no /dev/fuse in this container: hand test L-1")
    if box.run("command -v fusermount3").rc != 0:
        raise CaseSkip("no fusermount3 in this image: hand test L-1")
    libfuse2 = box.run("ldconfig -p | grep -F 'libfuse.so.2'")
    expect(libfuse2.rc != 0, "libfuse2 is installed, the case proves nothing: %r" % libfuse2)
    # The runtime mounts the image at $TMPDIR/.mount_*. It goes under /tmp, a user's default: newer Ubuntu hosts
    # (AppArmor 4.1+) confine /usr/bin/fusermount3 by path, even in this privileged container, to mount points under
    # $HOME, /mnt, /media, /run/user/<uid> and /tmp, and refuse any other with EACCES ("mount failed: Permission
    # denied"). The FUSE mount is a mount of its own, so the tmpfs's noexec does not reach it.
    box.sh("mkdir -p /tmp/fuse-user && chown tuser /tmp/fuse-user")
    r = box.run("cd /tmp/fuse-user && env -u APPIMAGE_EXTRACT_AND_RUN TMPDIR=/tmp/fuse-user QT_QPA_PLATFORM=offscreen "
                "timeout -k 10 120 %s --self-test-init" % image, user="tuser", timeout=180)
    expect("libfuse.so.2" not in r.out + r.err, "the AppImage asked for libfuse2: %r" % r)
    if r.rc != 0:
        # Say which mount-point rule refused it, so a failure names its cause (D68 is about libfuse2, not the LSM).
        profile = box.run("grep -s fusermount3 /sys/kernel/security/apparmor/profiles").out.strip()
        expect(False, "the AppImage did not start without libfuse2: %r (AppArmor fusermount3 profile: %s)"
               % (r, profile or "none visible here"))
    lines = [l for l in r.out.splitlines() if l.strip().startswith("{")]
    expect(lines, "no self-test report: %r" % r)
    report = json.loads(lines[-1])
    expect(report.get("exposed") is True and report.get("painted") is True, "the window was not shown: %r" % report)
    modules = report.get("modules") or []
    expect(any("/.mount_" in m for m in modules), "the window did not run from a FUSE mount: modules %r" % modules[:10])


@case("appimage_uninstall_extract_mode")
def appimage_uninstall(box):
    """v1.1 WS-15 (INV-124): the terminal command the window shows runs as shown. `sudo <AppImage> --uninstall`
    reaches the EMBEDDED program (AppRun forwards it as it forwards --install; the window executable would refuse it
    with "run this with the console executable"): the unit, the program and the policy go, the settings stay; with
    --remove-settings they go too. Extract mode, as appimage_install_extract_mode."""
    machine = box.sh("uname -m").strip()
    if machine not in NAMES:
        raise CaseSkip("no AppImage is built for %s" % machine)
    image = DIST + "/" + NAMES[machine][0]
    if not box.exists(image):
        raise CaseSkip("no release AppImage mounted (driver.py --dist)")
    run = "mkdir -p /var/tmp/appimage && cd /var/tmp/appimage && TMPDIR=/var/tmp/appimage APPIMAGE_EXTRACT_AND_RUN=1 %s %s"
    gone = ("/usr/local/lib/systemd/system/" + UNIT, INSTALLED, "/usr/share/polkit-1/actions/org.aethersdr.aetherserialbridge.policy",
            "/usr/share/polkit-1/actions/agency.cloaked.aetherserialbridge.policy")
    for extra in ("", " --remove-settings"):
        r = box.run(run % (image, "--install"), timeout=300)
        expect(r.rc == 0 and box.active() == "active", "the AppImage's --install: %r" % r)
        res = box.apply(CONFIG_A)
        expect(res.get("outcome") == "Ok", "apply: %s" % res)
        r = box.run(run % (image, "--uninstall" + extra), timeout=300)
        expect("run this with the console executable" not in r.out + r.err,
               "--uninstall%s reached the window executable: %r" % (extra, r))
        expect(r.rc == 0, "the AppImage's --uninstall%s: %r" % (extra, r))
        for p in gone:
            expect(not box.exists(p), "--uninstall%s left %s" % (extra, p))
        if extra:
            expect(not box.exists(DATA), "--uninstall --remove-settings left %s" % DATA)
        else:
            expect(box.exists(DATA + "/config.json"), "--uninstall removed the settings without being asked")
