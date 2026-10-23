# What's new

## v1.2

The first release from the AetherSDR organisation.

**Linux: the polkit identifiers moved to `org.aethersdr`**

- The project moved into the AetherSDR organisation, and the polkit policy and action are now
  `org.aethersdr.aetherserialbridge.policy` and `org.aethersdr.aetherserialbridge.helper` (they were
  `agency.cloaked.aetherserialbridge.*`).
- **If v1.1.0 or earlier is installed, uninstall it, then install this release.** Your settings are kept. Install,
  update and uninstall also remove the old policy file if it is still there. See [Linux](docs/linux.md).

**Windows**

- **Settings → Apps → Installed apps** names AetherSDR as the publisher.

## v1.1

The window now does the whole job on both Windows and Linux. You can install, update and uninstall the bridge
without opening a terminal.

**Install, update and uninstall from the window**

- **Install the bridge service.** On a machine with nothing installed, one button installs the service and the
  program, applies your ports and starts the bridge. On Windows it also creates the firewall rules. On Linux your
  desktop asks for an administrator's password once.
- **Update to this build.** When the installed bridge is a different build from the window's, the window says so and
  offers to update it. Save & apply asks too: **Update** replaces the installed program and keeps your settings,
  **No, keep it** applies your settings to the installed program, and **Cancel** changes nothing.
- **Uninstall…** lists the ports that go down and any connected client. It removes the bridge and keeps your settings,
  unless you tick **Also remove my settings**.
- **Windows:** the bridge appears in **Settings → Apps → Installed apps**, so you can uninstall it there the usual way.
  A standard user's window has **Restart as administrator**, which reopens it with administrator rights after
  Windows' prompt.
- **Linux:** the window (the AppImage, for x64 desktops and for the Raspberry Pi desktop) no longer needs `libfuse2`.
- `--uninstall --remove-settings` also removes the settings from the command line.

**The terminal interface**

- Press `?` for a help page. It explains every key and every field, with an example for each field. While you edit a
  port, the line under the form explains the field you are on.
- When a different build is installed, the TUI asks on open whether to update it to this one. `y` updates and keeps
  your settings, while `n` or Esc changes nothing. Press `u` later to be asked again. A read-only TUI shows the exact
  command to run instead.
- A long service state now wraps onto the next line instead of running off the screen.

**Clearer help in the window**

- The **Allowed** field now has an example under it, and says what the list does on your system. On Windows, while
  Windows Firewall is on, the port's rule lets in only those addresses. On Linux the list is recorded but not
  enforced, so use the system firewall.
- **Device**, **Listen on** and **TCP port** have tooltips. On Linux the Device tooltip recommends the
  `/dev/serial/by-id/` name.
- **Windows:** the window has its own icon.

**The service**

- A stopped bridge stays stopped: Windows and systemd no longer restart it over and over. It starts again only when
  you start it.
- **Windows:** when the service stops because of its firewall check, `bridge.log` says why.

More detail on each change is in the guides: [Windows](docs/windows.md) and [Linux and Raspberry Pi](docs/linux.md).
