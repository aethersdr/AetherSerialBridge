# Security Policy

## Reporting a Vulnerability

If you discover a security vulnerability in AetherSerialBridge, please report it
**privately** rather than opening a public issue.

**Email:** Send details to the repository owner via GitHub private messaging,
or use [GitHub's private vulnerability reporting](https://github.com/AetherSDR/AetherSerialBridge/security/advisories/new).

Please include:
- A description of the vulnerability
- Steps to reproduce
- Potential impact

We will acknowledge your report within 72 hours and work with you on a fix
before any public disclosure.

## Scope

AetherSerialBridge listens on TCP ports and runs as a privileged service on the
computer serial equipment is cabled to. Security concerns include:

- Who can reach a bridge port: the bind address, the allowed-clients list and the
  Windows firewall rules the bridge creates
- Malformed or hostile input on a bridge port, in the configuration and status
  files, or in a request to the privileged helper
- Privilege boundaries: the Windows service and the elevated window, the polkit
  helper and the systemd unit on Linux, and what each trusts on disk (data-folder
  trust, lock files)
- A device keyed or powered by something other than a connected client (DTR/RTS)
- Sensitive data exposure (e.g. client addresses or configuration in logs)

[`docs/architecture.md`](docs/architecture.md) describes what runs elevated and
what it trusts.

## Supported Versions

Only the latest version on the `main` branch receives security fixes.
