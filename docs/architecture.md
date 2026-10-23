# How it works

This page is for the technical reader: what runs where, how a change is applied, and what the bridge
trusts. Operators who only want to set it up can start with [windows.md](windows.md) or
[linux.md](linux.md).

## The executables

The bridge is written in C++ with Qt 6. Each platform has a program with a window and a program without
one.

| | Windows | Linux |
|---|---|---|
| Window | `AetherSerialBridge.exe` | `aether-serial-bridge-gui-x86_64.AppImage` / `-aarch64.AppImage` |
| Console program | `aether-serial-bridge.exe` | `aether-serial-bridge-linux-x64` / `-arm64`, installed as `/usr/local/bin/aether-serial-bridge` |

The **console program** is the only one that moves bytes. It is the service, the command line and the
terminal interface (TUI). The **window** never contains the bridge engine. It reads the configuration and
the status, and it changes them through the same code the command line uses:

- On Windows the window does the work itself, and it can do so only when it runs as administrator.
  Started normally, it is read-only and builds nothing that could change the system; **Restart as administrator**
  starts the same program again through UAC (`ShellExecuteExW` with `runas`, fixed arguments). The window installs
  the `aether-serial-bridge.exe` beside it, and uninstalls, in process.
- On Linux the window always runs as your own user. Day-to-day changes go through polkit:
  `pkexec /usr/local/bin/aether-serial-bridge --helper`. The installed console program does the work as
  root. The window sends it one request and reads back one answer. Polkit asks for an administrator
  password every time (`auth_admin`, no remembered authorisation).
- **The Linux window's install, update and uninstall (the staged helper).** These cannot use the installed program
  (there is none yet, or it is the build being replaced). The window runs `pkexec /bin/sh -c <a fixed script>` under
  polkit's generic `org.freedesktop.policykit.exec` action, which is why the prompt names `/bin/sh`. It writes the
  program inside its AppImage, then the request, on the script's stdin. As root, the script checks that `/`, `/usr`,
  `/usr/local` and `/usr/local/lib` are writable by root only, copies everything into a new root-only folder under
  `/usr/local/lib/aether-serial-bridge`, checks the program's SHA-256 against the digest given on its command line
  (the digest compiled into the window), and only then runs that copy as `--helper` with the request. Anything wrong
  (a short or long payload, a different digest, an unsafe folder) runs nothing, removes the folder and prints one
  `asb-stage:` line; the window turns that line into its message. The installed helper refuses `install` and
  `uninstall`: only a staged copy runs them.

**Build identity.** Both OSes tell builds apart by the program's SHA-256: the window carries the digest of the
program it installs (beside it in the zip, or inside its AppImage), and the installed program is "this build" only
when its digest matches. Another build is shown as such, and Save & apply asks before it replaces it (Update, No,
keep it, Cancel). Every program from v1.1 on also carries a build stamp with its version, which is used only for
wording ("an earlier build, before 1.1"), never to decide.

Started with no arguments, each program picks its own mode:

| Program | Display available | No display, in a terminal | Neither |
|---|---|---|---|
| Window program | the window | Linux: the TUI, read-only | help text |
| Console program | the TUI | the TUI | help text |

`--gui` and `--tui` force a mode. The Windows window program has no TUI. Use `aether-serial-bridge.exe
--tui` there.

The Windows executables and the Linux console programs are statically linked: no Qt DLLs, no plugins
folder, no runtime to install. The Linux window is an AppImage with its own copy of Qt inside.

## One port, one listener

For every enabled port in the configuration, the service opens one TCP listener on the configured address
and port. Ports are independent. If one cannot listen (the address isn't up yet, or the TCP port is taken),
its status shows the error and it retries every 10 seconds. The other ports keep working.

## A session

1. **A client connects.** This can be AetherSDR, `nc`, or a vendor utility through a virtual COM port.
2. **The serial port is opened now, not before.** While nobody is connected, the port stays closed, so a
   vendor utility or firmware updater can use it. The serial settings (baud, format, DTR, RTS) are set
   before the port opens. If any of them can't be set, the open fails. A failed open is tried three times,
   250 ms apart. If all three fail, the client is dropped and the status gives the reason.
3. **Bytes are copied both ways, unchanged.** Nothing is added, removed or translated.
4. **The session ends** when either side closes. A client that closes after sending a command still has
   that command delivered first, so `printf 'PS0;' | nc …` reaches the device. Unplugging the adapter or a
   serial error also ends the session. The port goes back to *Listening*, and the status keeps the last
   error, for example `device error: …`.
5. **A stalled side ends the session.** If the serial device or the client accepts no data for 5 seconds
   while bytes are waiting for it, the session ends with `stalled: … accepted no data for 5 s`.

### A new client replaces the old one

Only one client can own a serial device. Two programs polling the same amplifier would mix up each other's
commands and replies. When a second client connects, the bridge closes the first client's connection and
its serial port. It then opens the port fresh for the new client. The log names both addresses.

Why replace the old client instead of refusing the new one? A client that crashes, sleeps or loses Wi-Fi
often leaves its TCP connection half-open for minutes. If the bridge refused new clients, the port would be
locked until that dead connection timed out. With replacement, reconnecting AetherSDR always works. The cost
is that a second program can kick AetherSDR off. That is why these docs say "one client per port". TCP
keepalive is on, so a half-open connection is noticed in the end even if nobody replaces it.

## Raw TCP only

The bytes on the TCP connection are exactly the bytes on the serial wire. AetherSDR's network mode speaks
this for the devices it supports. It is also the only safe choice for binary protocols such as ACOM's,
because telnet framing would double every `0xFF` byte.

RFC 2217 (telnet COM-port control), changing the baud rate from the client, and remote DTR/RTS control
are **not goals of this bridge**:

- DTR and RTS are fixed per port in the configuration and are off by default. Some amplifiers wire those
  lines to power or PTT. A client that can reach the port cannot change them.
- With no telnet mode, the ACOM failure above can't happen through a wrong setting.

Devices that need RFC 2217, such as the SPE Expert's remote power-on, should use
[ser2net](ser2net.md).

## Configuration files and the apply model

The configuration is owned by the bridge's tools. Edit it with the window, the TUI or the port commands,
not with a text editor. A hand-edited `config.json` is not supported. On Windows, the firewall would not
match it.

| File | Meaning |
|---|---|
| `config.json` | The **committed** configuration: what the service runs. Only an apply replaces it. |
| `config.pending.json` | **Saved, not applied** edits. Port commands and an unticked Save & apply write here. `--apply` (or Save & apply) applies them, and `--discard-pending` drops them. |
| `start.token` | The activation token (see below). |
| `status.json` | Written by the running service: per-port state, client, byte counts, last error and the acknowledgement fields. |

A configuration that loads is also validated before anything uses it. Besides the per-field rules, it
refuses:

- shortened or hex IPv4 addresses (such as `10.1` or `0x0a.0.0.1`) as a bind address or allowed client;
- RTS/CTS and XON/XOFF flow control together;
- firewall keywords other than those listed under [Firewall](#firewall).

### What an apply does

Save & apply, `--apply`, `--install` and `--start` each run as **one guarded operation**. Only one runs
at a time across all windows, terminals and the Linux helper. In plain terms, an apply:

1. Takes the **apply lock**.
2. Checks the installation. A service this program didn't create stops the operation with nothing
   changed.
3. Checks that the data folder is trusted (see below). If it isn't, nothing is changed.
4. **Revokes the activation token**, so nothing can start the bridge from here on.
5. Blocks the service from starting (Windows: start type *Disabled*; Linux: systemd *mask*), then asks it to
   stop.
6. **Proves the old bridge has stopped** by taking the engine lock, which a running bridge always holds. It
   waits up to 30 seconds.
7. Writes the new settings to `config.pending.json` and flushes them to disk.
8. Writes the new settings to a temporary file and reads them back. On Windows it then **updates the
   firewall from exactly those bytes**.
9. **Commits:** renames the temporary file over `config.json`. The service never sees a configuration that
   the firewall was not updated for.
10. Writes a fresh activation token, allows starts again, starts the service.
11. Waits up to 15 seconds for the service to **acknowledge** in `status.json`. The service reports its
    process id, a SHA-256 of the configuration it loaded and the token it was started with. All three must
    match.

If a step fails, the result says how far the operation got:

| Result | What it means |
|---|---|
| Saved and applied | The new settings are running. |
| Not changed | Nothing was changed. The message says why: a service this program didn't create, an untrusted folder, or the lock was busy. |
| Saved, not applied | Your edits are in `config.pending.json`. The service section of the message says what state the bridge is in. |
| Saved, did not start | The new settings are committed and the firewall matches them, but the bridge did not start, or it rejected the configuration. |
| State unknown | The bridge could not be proven stopped within 30 s, or it did not confirm loading the new settings within 15 s. The message lists what was already done. |

The window and the TUI only treat your edits as saved when the result says they reached the disk.
Otherwise the edits stay open in the editor.

### Fail-closed

The token is revoked at the start of every change, and a new one is written only after a successful
commit. The service refuses to start without a valid token. So after `--stop`, after a failed apply, or
after a crash in the middle of one, the bridge **stays off, across reboots too**, until a `--start` or a
Save & apply succeeds. In the log, this shows as exit code 6 ("not activated") with one line saying why. A start
that finds no token, at boot or after a restart the service manager had already scheduled, is not a failure: the
service ends as a normal stop and is not restarted (Windows reports no error code; the Linux unit counts exit 6 as
a clean exit).

`--start` re-applies the committed `config.json`. On Windows it updates the firewall from that file and
then starts the service. Use it after fixing whatever made an apply fail.

### Operations that take a while

Native calls such as the Windows firewall (WMI/CIM), the service manager and disk flushes can't be
cancelled. When one runs past its deadline, you get a "still processing" notice instead of a result. The
operation keeps its locks until the call returns, then finishes and reports. The window and the TUI refuse
to close while an operation is outstanding.

On Windows, most of an apply's time is spent on firewall work: about one second, plus three to four seconds
per port rule. An apply with several ports can take tens of seconds.

## Locks

| Lock | Windows | Linux | Held by |
|---|---|---|---|
| apply lock | `%ProgramFiles%\AetherSerialBridge\apply.lock` | `/run/aether-serial-bridge.apply.lock` | an operation, for its whole run |
| engine lock | `%ProgramFiles%\AetherSerialBridge\engine.lock` (+ `engine.lock.id`) | `/var/lib/aether-serial-bridge/engine.lock` (+ `engine.lock.id`) | the running service, for its whole life |

Both are ordinary exclusive file locks, so the kernel releases them when the process dies. Each engine lock
holds a random id, recorded at install time in `engine.lock.id`. The service and the apply both check it.

On Linux the engine lock file can be read by any local user. A local user could therefore hold it and keep
applies from completing. This blocks the bridge from being changed, but it can never make the bridge run
the wrong settings. The error message names the process holding it.

## Data folder trust

The service runs with high privileges (Windows: LocalSystem) and trusts its configuration. So the data
folder must be writable only by administrators (Windows) or root (Linux).

| | Windows | Linux |
|---|---|---|
| Data folder | `%ProgramData%\AetherSerialBridge` | `/etc/aether-serial-bridge` |
| Must be owned by | Administrators / SYSTEM | root, not group- or world-writable |

Every viewer (the window, the TUI, `--status`, `--show-config`) only reads. If the folder is not trusted,
viewers say "Config not loaded: the data folder isn't trusted." and the service refuses to start (exit code
3). Changes refuse too, until you **repair** the folder:

- Window: **Repair…**. TUI: `R`.
- Command line: `--repair` (asks first; add `--yes` to skip the question), or `--install --force-recreate`
  / `--apply --force-recreate`.

Repair stops the bridge and blocks it from starting. It then **moves the untrusted folder aside**, with
everything in it, and creates a fresh one. Nothing is deleted. The message names where the old folder went,
so you can review the `config.json` in it. Symbolic links and junctions in place of the folder or its files
are refused.

## Firewall

**Windows.** The bridge manages inbound allow rules in the Windows Defender Firewall group
**`Aether Serial Bridge`**. Each enabled port gets one rule: TCP to that port, for the installed
`aether-serial-bridge.exe`, from the port's **Allowed clients** (blank means any address), on every network
profile. Allowed clients are IP addresses, prefixes such as `192.168.1.0/24`, ranges `a-b`, or one of
these firewall keywords: `Any`, `LocalSubnet`, `LocalSubnet4`, `LocalSubnet6`, `DNS`, `DHCP`, `WINS`,
`DefaultGateway`, `Internet`, `Intranet`, `IntranetRemoteAccess` and `PlayToDevice`.

New rules are created disabled, completed and checked, and only then enabled. The rules from the previous
apply are removed afterwards. All of this happens in process, through WMI/CIM. The bridge starts no
PowerShell or `netsh`.

The group is **reserved for the bridge**. The running service re-reads its group before it opens any
listener and then at least every 30 seconds. If any enabled rule in the group does not match what the last
apply wrote, the service stops itself with exit code 7. That includes a rule an administrator added or
edited by hand, or one delivered by Group Policy. Windows restarts it, it stops again, and the bridge
stays down until an apply rewrites the rules. While the installed bridge is down this way, the window says
which rule does not match.

**Linux.** The bridge does not touch the firewall. Allowed clients are recorded in the configuration but
**not enforced**. Every interface says so. Use your own firewall: [linux.md](linux.md#firewall).

## Status and acknowledgement

The service writes `status.json` at most once a second. It goes to a temporary file that is then renamed
into place, so a reader never sees half a file.

- Windows: `%ProgramData%\AetherSerialBridge\status.json`.
- Linux: `/run/aether-serial-bridge/status.json`, which is cleared at reboot.

Each port shows one of these states: *Listening*, *Connected* (with the client's address), *Error* or
*Stopped*. The status also holds byte counts in both directions and the last error. The window and the TUI
show a Listening port's last error as "Listening · last: …". `--status` prints the error only for a port in
the *Error* state. A dropped client's reason is always in the log. `--status` compares
the reported configuration digest with `config.json` on disk. It also says when the bridge is not activated
(no token), and whether saved, not applied settings are waiting.

If the configuration is invalid, the service puts the reason in the status (`Config error: …`). An apply
reports that as "did not start".

## Logs

- Windows: `%ProgramData%\AetherSerialBridge\bridge.log`. At 1 MB it rolls over to `bridge.log.1`, so the
  logs use about 2 MB at most. The window shows it under **Show log**.
- Linux: the systemd journal, `journalctl -u aether-serial-bridge`.

## The service

| | Windows | Linux |
|---|---|---|
| Name | service `AetherSerialBridge` | systemd unit `aether-serial-bridge` |
| Program | `"%ProgramFiles%\AetherSerialBridge\aether-serial-bridge.exe" --service` | `/usr/local/bin/aether-serial-bridge --service` |
| Unit / account | LocalSystem | `/usr/local/lib/systemd/system/aether-serial-bridge.service`, `DynamicUser=yes` in the `dialout` group |
| Restarts after a crash | after 5 s, 5 s, then 30 s | `Restart=on-failure`, after 5 s (`SuccessExitStatus=6`: "not activated" is no failure) |

`--service` runs only when the service manager started it. It refuses if you start it by hand. On Linux it
checks that its own control group is exactly `0::/system.slice/aether-serial-bridge.service`, which needs
cgroup v2. Hosts with cgroup v1 or a hybrid layout are not supported.

The Linux unit also sets `NoNewPrivileges=yes`, `ProtectSystem=strict`, `ProtectHome=yes` and
`PrivateTmp=yes`. Device access is not restricted beyond the file permissions on the serial ports
(`root:dialout 0660`). For local settings use a drop-in (`sudo systemctl edit aether-serial-bridge`):
`--install` rewrites the main unit file.

Service exit codes, as the service manager reports them:

| Code | Meaning |
|---|---|
| 1 | an internal fault. The service manager restarts it. |
| 2 | `--service` was not started by the service manager |
| 3 | the data folder is not trusted (run Repair) |
| 4 | the engine lock is busy, missing or does not match its record (run `--install` or Repair) |
| 5 | the process guard could not be set up |
| 6 | not activated: stopped by Stop, or an apply did not finish (run Start or Save & apply). Not a failure: it is not restarted, and Windows shows it as a normal stop with no error code. |
| 7 | Windows: a rule in the `Aether Serial Bridge` firewall group does not match the last apply |

## What runs elevated

| Action | Windows | Linux |
|---|---|---|
| Viewing (window, TUI, `--status`, `--show-config`, `--list-ports`) | no | no |
| Any change (Save & apply, Start, Stop, Repair, port commands, install, update, uninstall) | administrator (the window: Restart as administrator, UAC) | root, through `sudo` or polkit (the window's install, update and uninstall: the staged helper above) |
| The service | LocalSystem | a dynamic user in `dialout` |
| `--run --config <file>` (diagnostic) | no | no (needs `dialout` for the serial port) |

Processes that change the system, and the service itself, start no other programs. A runtime guard
enforces that: a Windows job object, a seccomp filter on Linux. The product starts a process in exactly these places:

| Where | Program | What it starts | When |
|---|---|---|---|
| `src/gui/PkexecClient.cpp` | the Linux window | `pkexec /usr/local/bin/aether-serial-bridge --helper`, and `pkexec /bin/sh -c <the staging script> …` | one per click |
| `src/platform/win/SelfElevate.cpp` | the Windows window and console program | its own program, through UAC (`runas`), with fixed arguments | **Restart as administrator** in a standard user's window; `--uninstall-from-apps` (the Settings → Apps entry) before it elevates |

The elevated Windows window runs under the same job object as before; a standard user's window runs under a job
that allows one more process (it builds nothing that changes the system).

## Diagnostic runs

`--run --config <file>` runs the bridge in the terminal against a scratch configuration. It is for testing
a device without touching the service:

- The file must be outside the data folder, in a folder that only you (or root/an administrator) can
  write.
- Every listener is moved to loopback (`127.0.0.1` or `::1`) with a warning, so only programs on the same
  machine can connect.
- It refuses to run from the installed program. Use the downloaded file.
- It takes no locks and no token, and touches no firewall rule. `status.json` is written next to the
  scratch file, and `--status --config <file>` reads it.

If the service already has the same serial port open, the diagnostic run can't open it either.

## The window and the theme

The window is AetherSDR's own window code (the "Canon" workspace window), vendored unchanged except for two
small documented patches. By default it follows AetherSDR's active theme. When the window opens, it reads
the theme name from AetherSDR's settings database **read-only** and loads that theme's file after checking
its structure. If anything is missing, ambiguous or invalid, it uses Default Dark. It never writes to
AetherSDR's folders. As with any second reader of an SQLite database, opening it can update AetherSDR's
`-shm` file.

