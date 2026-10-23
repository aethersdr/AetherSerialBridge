Aether Serial Bridge for Windows
================================

Puts serial devices (amplifiers, tuners) on the network for AetherSDR. Each device gets its own TCP port.
Windows 10 version 1903 or later, or Windows 11, x64.

This folder holds two programs. Keep them together, in the same folder:

  AetherSerialBridge.exe     the window
  aether-serial-bridge.exe   the console program: the Windows service, the command line (--help) and a
                             terminal interface (--tui)

Both are self-contained: no DLLs and no separate installer.

Set it up
---------
1. Unzip anywhere, for example to your Downloads folder.
2. Open AetherSerialBridge.exe. Opened normally, the window only shows the state. Click "Restart as administrator"
   and approve the Windows prompt: the window reopens and can change the bridge.
3. Add a port for each device ("+ Add port"), then click "Install the bridge service". It copies
   aether-serial-bridge.exe to %ProgramFiles%\AetherSerialBridge (and checks the copy against the SHA-256 the
   window was built with), installs the AetherSerialBridge service and its firewall rules, adds Aether Serial
   Bridge to Settings > Apps, and starts the bridge.
4. After that, change ports and click "Save & apply". Keep the unzipped folder: the window runs from there.

Settings and status live in %ProgramData%\AetherSerialBridge.

Update
------
Unzip the new release to a new folder and open its AetherSerialBridge.exe (Restart as administrator if it is
read-only). It sees the installed bridge is another build and shows "Update to this build": click it and confirm.
Your settings are kept; the bridge restarts. Then use the new folder and delete the old one.

Uninstall
---------
In the window (as administrator), click "Uninstall...". Or use Settings > Apps > Aether Serial Bridge > Uninstall,
or "aether-serial-bridge.exe --uninstall" from a terminal opened with Run as administrator. Your settings are kept
unless you choose to remove them.

Terminal interface
------------------
aether-serial-bridge.exe --tui, in a terminal, has the same operations as the window. To change anything, open the
terminal with Run as administrator first; otherwise it is read-only and says so. Press ? for help.

Anyone who can reach a bridge port can operate the device: allow only the AetherSDR computers, keep it on the
LAN or a VPN and never forward it from the internet.

Verify the download against SHA256SUMS from the release page:  Get-FileHash <zip>  (this release is unsigned, so
Windows SmartScreen may warn the first time).

Documentation: https://github.com/AetherSDR/AetherSerialBridge/blob/main/docs/windows.md
Licence: GNU GPL v3 (LICENSE.txt). Third-party components and their licences: THIRD-PARTY-NOTICES.txt.
