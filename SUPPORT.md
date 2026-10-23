# Getting Help with AetherSerialBridge

AetherSerialBridge is volunteer-maintained open-source software, part of the
AetherSDR project. There is no SLA and no guaranteed response time — but bug
reports with the status output and the log attached get priority.

## Where to go

- **Setting it up:** the guides — [Windows](docs/windows.md), [Linux and
  Raspberry Pi](docs/linux.md), [supported devices](docs/devices.md); each ends
  with a troubleshooting table.
- **General questions / discussion:** AetherSDR's GitHub Discussions —
  https://github.com/aethersdr/AetherSDR/discussions
- **Bug reports:** file an issue with the bug-report template. Attach the output
  of `aether-serial-bridge --status` and the log: on Windows
  `%ProgramData%\AetherSerialBridge\bridge.log` (the window's **Show log**), on
  Linux `journalctl -u aether-serial-bridge`.
- **Feature requests:** file an issue with the feature-request template.
- **Security issues:** **do not** open a public issue. See
  [`SECURITY.md`](SECURITY.md) for the private-disclosure process via
  GitHub Security Advisories.

## Real-time chat

Project lead (Jeremy KK7GWY) operates from DN18 in the US Pacific
Northwest. For real-time questions, ping in the GitHub Discussions
thread relevant to your topic and you'll usually get a same-day reply.

## Commercial / consulting

Email the project lead at `kk7gwy@aethersdr.com` — happy to discuss
custom integration work, sponsored development of a feature you need,
or training on the codebase.

## What gets you the fastest fix

1. A reproducible bug report on a current release (latest tag).
2. `aether-serial-bridge --status` and the log from the moment the bug occurred.
3. The device (make, model, firmware), its serial adapter, and the OS.
4. Whether the device works over the same cable with its own software or a
   terminal program — helps narrow cable and adapter vs. bridge issues.
