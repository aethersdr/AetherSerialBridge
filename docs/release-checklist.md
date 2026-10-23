# Release checklist

The maintainer signs this off before a draft release is published. `release.yml` builds and gates everything on
a `v*` tag push and creates a **draft** release; nothing is public until the steps below are done and the draft
is published by hand. A dry run of the same workflow (`gh workflow run release.yml --ref <branch>`) builds and
gates everything and uploads the files as workflow artifacts only.

Copy this list into the release's tracking issue and tick it there.

## 1. Before tagging

- [ ] `main` is green: every `ci.yml` leg, including the INV-88 control (`windows`: "the release self-test rejects a
      GUI with a dynamic qwindows").
- [ ] `qt-static.yml` has run on `main` since the Qt pin, a profile or a build script last changed (it also runs
      weekly), so the tag's run restores every static Qt from the Actions cache instead of building it (a cold build
      works too, it only takes longer).
- [ ] The repository variable `ASB_ARM64_RUNNERS` is not `false`, so the release has its arm64 files. If it is `false`,
      the release ships without `aether-serial-bridge-linux-arm64` and `aether-serial-bridge-gui-aarch64.AppImage`: say
      so in the release notes.
- [ ] A `release.yml` dry run on the commit to be tagged is green on every job: it runs the arm64 audits and the
      systemd suite in release mode on the hosted arm64 runner too.
- [ ] In that dry run, the AppImage gates (v1.1 INV-121) are green on both architectures and their controls fail as
      intended: `release_audit.py digest` (the window's compiled program digest is the embedded program's, which is
      the release program) and its control (`digest-control`: a window built without the release program's digest is
      refused, x64), `glibc-tree` (every ELF file in the image needs GLIBC_2.35 at most, the worst offender printed)
      and its control (`tools/glibc-tree-control.sh`: a planted plugin needing GLIBC_2.38 is found), and `stamp` (the
      build stamp of this version) on every shipped executable. `appimage_starts_without_libfuse2` passed, or its
      SKIP reason is in the log and a hand check on a desktop without `libfuse2` covers it.
- [ ] In that dry run, on both architectures, the v1.1 staged-route cases (WS-15) are green in release mode: every
      `bootstrap_*` case (the release program as the payload), `v10_installed_day_to_day`,
      `v11_installed_helper_refuses_lifecycle_ops`, `polkit_exec_defaults_bookworm` and `_fedora`, and
      `appimage_uninstall_extract_mode` (the AppImage's `--uninstall` reaches the embedded program). CI has no v1.0
      program fixture, so the v1.0 cases SKIP there: run them by hand against the pinned fixture
      (`tests/systemd/fixtures.env`, `driver.py --fixtures <dir>`) and record that they passed.
- [ ] The version in `CMakeLists.txt` (`project(... VERSION x.y.z)`) is the version to tag. The tag must be
      `vx.y.z`: the release gates compare `--version` with the tag and fail otherwise.
- [ ] `cmake/qt-pin.env` and `packaging/qt-static/qt-static.env` name the same Qt; `qt-static.yml` has run since
      either changed (otherwise the tag's run builds static Qt cold, 25–60 min per profile).
- [ ] `packaging/THIRD-PARTY-NOTICES.txt` still lists what the executables contain (Qt and its bundled
      third-party code, FTXUI, AetherSDR files, Inter, SQLite). Reviewed by: ________
- [ ] The operator docs (`README.md`, `docs/*.md`) describe this release.

## 2. Hardware re-runs on the release artifacts

The dry-run artifacts of the tagged commit (or the draft's files) are what runs here, never a development build.
Record the result and the artifact SHA-256 for each.

- [ ] **HW 5: DTR/RTS at open** (logic analyser or LED, FTDI adapter), on Windows (the zip) and on Linux
      (`aether-serial-bridge-linux-x64` or `-arm64`).
- [ ] **HW 8: reboot with auto-start**; then a failed apply followed by a reboot stays down (a Windows PC,
      and a Raspberry Pi with the systemd unit).
- [ ] **The Pi window** (`aether-serial-bridge-gui-aarch64.AppImage`) on a 64-bit Bookworm desktop or VNC: it opens
      from the file manager without `libfuse2`, and Save & apply asks polkit.
- [ ] **Install, Update, Uninstall from the window** on a clean Fedora KDE desktop and on the Pi: one polkit
      prompt each, naming `/bin/sh`; Cancel changes nothing.
- [ ] **Apps & features Uninstall** on a test Windows account: the confirmation in front, UAC, the entry
      gone, the program folder gone after the restart.
- [ ] **HW 9: the Linux window** (the AppImage): the real polkit prompt, cancel, Save & apply, untick → deactivate →
      the window still works; the Pi TUI over SSH at 80×24, 40×20 and with `NO_COLOR=1`.
- [ ] **Save & apply from the unzipped zip on a station PC** (the window, elevated): a PC that runs the bridge day to
      day, so it normally has an older build installed, and Save & apply asks "The installed bridge is a different build
      … Update it to this build?". **Answer Update** for this step (the question's default, "No, keep it", keeps the old
      program by design, and the SHA-256 check below would then fail). Afterwards `%ProgramFiles%\AetherSerialBridge`
      holds exactly `aether-serial-bridge.exe`, `apply.lock`, `engine.lock` and `engine.lock.id`, and the installed
      exe's SHA-256 is the zip's `aether-serial-bridge.exe`. (CI proves the CLI install path on the unzipped tree; the
      window's path needs an interactive, elevated desktop.)
- [ ] **Station check: the service with the elevated window open** (the outage of 2026-10-16: its root cause is still
      open, and this is the only check of it on real hardware). On a station that runs the bridge day to day, at the
      next elevated use of the window there, even when that use is not for a release: record everything below (in the
      release's tracking issue, or in the incident's issue). Until one run passes, this step stays on every release.
      - **Before opening the window:** `sc.exe query AetherSerialBridge` (state and `WIN32_EXIT_CODE` /
        `SERVICE_EXIT_CODE`), and the last 50 lines of `%ProgramData%\AetherSerialBridge\bridge.log`.
      - **With the window open, before doing anything in it:** the handles the window process holds,
        `handle.exe -p AetherSerialBridge.exe` (Sysinternals), or, without it,
        `Get-Process AetherSerialBridge | Select-Object Id,HandleCount,Path` plus the handle list from
        `handle.exe -a -p <Id>` when it can be fetched. Look in particular for handles under
        `%ProgramFiles%\AetherSerialBridge` and `%ProgramData%\AetherSerialBridge`.
      - **In the window:** Stop → Save & apply (answer **Update** if the build question appears) → Start. Then, with
        the window still open, `sc.exe stop AetherSerialBridge` and `sc.exe start AetherSerialBridge` twice; each
        time the service reaches RUNNING and is still RUNNING 10 s later.
      - **Then, still with the window open:** the service's start events from the System log with their codes,
        `Get-WinEvent -FilterHashtable @{LogName='System'; Id=7000,7009,7031,7034; StartTime=<when the window was
        opened>}`, the `sc.exe query AetherSerialBridge` exit codes, and `bridge.log` again.
      - **If the service does not start, or stops, while the window is open:** capture all of the above (handles,
        events, exit codes, `bridge.log`) **before closing the window**, then close it and run `sc.exe start
        AetherSerialBridge` once more and record whether it starts now. Do not publish the release until the cause is
        understood.
      - Pass: no 7000/7009/7031/7034 event for `AetherSerialBridge` since the window was opened, and every start
        reached RUNNING.

## 3. The draft release

- [ ] Every file of the draft matches `SHA256SUMS` (`sha256sum -c SHA256SUMS`; on Windows `Get-FileHash`).
- [ ] There is exactly one release (draft) for the tag; the publish job refuses when one already exists, so a
      re-run after a failed upload needs the stale draft deleted first.
- [ ] The draft holds exactly: `aether-serial-bridge-windows-x64.zip`, `aether-serial-bridge-linux-x64`,
      `aether-serial-bridge-linux-arm64`, `aether-serial-bridge-gui-x86_64.AppImage`,
      `aether-serial-bridge-gui-aarch64.AppImage`, `SHA256SUMS`.
- [ ] The release notes say the files are unsigned and how to verify them.
- [ ] Published by: ________ on ________

Signed off: ________ (maintainer) Date: ________
