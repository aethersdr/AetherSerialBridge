# Aether Serial Bridge documentation

Aether Serial Bridge puts serial-only station gear on the network. It runs on the computer the amplifier
or tuner is cabled to: a Windows PC, a Linux PC, or a 64-bit Raspberry Pi. Each serial port gets its own
TCP port, and [AetherSDR](https://github.com/aethersdr/AetherSDR) connects to that TCP port instead of a
local COM port. The bridge moves bytes unchanged in both directions. Each port has one client at a time,
and the serial port is opened only while a client is connected. It runs as a Windows service or a systemd
service. You configure it from a window, a terminal interface or the command line.

## Downloads

All files are on the [Releases page](https://github.com/AetherSDR/AetherSerialBridge/releases).

| File | For |
|---|---|
| `aether-serial-bridge-windows-x64.zip` | Windows 10 version 1903 or later, or Windows 11; x64: `AetherSerialBridge.exe` (the window) and `aether-serial-bridge.exe` (service, command line, terminal interface), with licence texts and a short readme |
| `aether-serial-bridge-linux-x64` | Linux x64: one static program (service, command line, terminal interface) |
| `aether-serial-bridge-linux-arm64` | 64-bit ARM Linux, such as a Raspberry Pi 3/4/5 or Zero 2 W on 64-bit Raspberry Pi OS |
| `aether-serial-bridge-gui-x86_64.AppImage` | The configuration window for an x64 Linux desktop. It contains the x64 program. |
| `aether-serial-bridge-gui-aarch64.AppImage` | The configuration window for a Raspberry Pi (64-bit Raspberry Pi OS with desktop). It contains the arm64 program. |
| `SHA256SUMS` | SHA-256 digests of every file above |

The release is not code-signed. Check downloads against `SHA256SUMS`: run `sha256sum -c --ignore-missing
SHA256SUMS` on Linux, or `Get-FileHash` on Windows.

## Guides

| Page | What's in it |
|---|---|
| [../CHANGELOG.md](../CHANGELOG.md) | What's new in v1.1 |
| [windows.md](windows.md) | Windows: the window ([set it up](windows.md#set-it-up), [its buttons](windows.md#the-windows-buttons), [Stop and auto-start](windows.md#stop-and-auto-start)), [update](windows.md#update-to-a-new-release), [uninstall](windows.md#uninstall) from the window or Settings → Apps, [the terminal interface](windows.md#terminal-interface), the service, the command line, files and firewall, troubleshooting |
| [linux.md](linux.md) | Linux and Raspberry Pi: install, adapters and by-id names, ports, [the terminal interface](linux.md#terminal-interface), [Stop and auto-start](linux.md#stop-and-auto-start), [the AppImage window](linux.md#the-window-x64-desktop-or-a-raspberry-pi-desktop) (install, update and uninstall from the window, and why polkit's prompt names `/bin/sh`), firewall, troubleshooting |
| [devices.md](devices.md) | Supported devices: serial settings, safe test commands, which bridge to use |
| [clients.md](clients.md) | Virtual COM ports for programs that can only open a local serial port |
| [ser2net.md](ser2net.md) | Aether Serial Bridge or ser2net: an honest comparison, and the ser2net setup in this repository |
| [architecture.md](architecture.md) | How it works: the executables, sessions, the apply model, data-folder trust, firewall, locks, status, what runs elevated |
| [release-checklist.md](release-checklist.md) | For maintainers: what is checked before a release is published |
| [images/README.md](images/README.md) | For maintainers: how the images in these pages are made from a demo configuration, and checked |
