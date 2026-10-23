# Contributing to AetherSerialBridge

Thanks for your interest in AetherSerialBridge! It is the AetherSDR project's
serial-to-network bridge: it puts serial-only station equipment (amplifiers,
tuners, rotators) on the network as raw TCP ports for AetherSDR, on Windows,
Linux and the Raspberry Pi. Community contributions are welcome.

This file is the **policy** side of contributing: what we accept, who
reviews what, and the ground rules. The **operational** side — architecture,
build, coding conventions — lives in [`AGENTS.md`](AGENTS.md) and
[`docs/architecture.md`](docs/architecture.md).

See [GOVERNANCE.md](GOVERNANCE.md) for project roles, decision-making, and
the RFC process for significant changes. AetherSerialBridge is part of the
AetherSDR project and follows the same rules.

---

## Quick Start

1. Browse [open issues](https://github.com/AetherSDR/AetherSerialBridge/issues) —
   issues labeled `good first issue` are great starting points.
2. Fork the repo and create a feature branch from `main`.
3. Implement the fix or feature (one issue per PR).
4. Open a pull request referencing the issue number (`Fixes #42`).

---

## Reporting Bugs

- Open a [GitHub issue](https://github.com/AetherSDR/AetherSerialBridge/issues/new/choose)
  with the bug-report template.
- Include: OS (Windows version, or Linux distribution and version, or
  Raspberry Pi OS), the bridge's version (`aether-serial-bridge --version`),
  the device (make, model, firmware) and its serial adapter.
- Attach the output of `aether-serial-bridge --status` and the log: on
  Windows `%ProgramData%\AetherSerialBridge\bridge.log` (the window's
  **Show log** shows it), on Linux `journalctl -u aether-serial-bridge`.
  Check them for addresses and names you would rather not post.
- Check existing issues first to avoid duplicates.

## Suggesting Features

- Open a GitHub issue with the feature-request template.
- Describe the problem you're solving, not just the solution.
- Name the device and what it needs (its serial settings, its protocol,
  what AetherSDR does with it).
- One feature per issue.

---

## Submitting Code

**Development tool:** the AetherSDR project is developed using
[Claude Code](https://claude.com/claude-code) as the primary development
environment. We **strongly encourage all contributors to use Claude Code** —
it has full codebase context via `AGENTS.md` and naturally produces code that
matches our conventions.

1. **Fork the repo** and create a feature branch from `main`.
2. **Read the [Constitution](CONSTITUTION.md).** (Canonical source:
   [`.specify/memory/constitution.md`](.specify/memory/constitution.md);
   the root [`CONSTITUTION.md`](CONSTITUTION.md) is a byte-identical
   mirror.) It is the AetherSDR Constitution with the same principle
   numbers: the clean-room rule, transmit-on-intent, boundary input
   validation, plus defensive engineering principles adopted from Cisco's
   [Foundry Constitution](https://github.com/CiscoDevNet/foundry-security-spec/blob/main/constitution.md)
   (Evidence Over Assertion, Surface Only What Survives, Atomic Claims,
   Demonstrated Fixes, Infra Sandbox, Operator Outranks Agents, Atomic
   Persistence). Your PR's commit message will cite the principle it
   honors (e.g. `Principle VII.` for a parser that refuses malformed input,
   or `Principle XIV.` for an atomic write).
3. **One issue per PR.** Keep changes focused and reviewable.
4. **Follow the coding conventions** in [`AGENTS.md`](AGENTS.md#c-style-guide).
5. **Test your changes** against a real serial device if possible, and say
   which one in the PR.
6. **Sign your commits** (required by branch protection — SSH or GPG; see
   AetherSDR's [Commit Signing](https://github.com/aethersdr/AetherSDR/blob/main/docs/COMMIT-SIGNING.md)
   guide).
7. **Open a pull request** against `main` with a clear description.

---

## Developer documentation

Everything you need to actually write the code:

| Document | Covers |
|---|---|
| [`AGENTS.md`](AGENTS.md) | The canonical project guide — the source every AI tool reads: build, conventions, CI, architecture overview |
| [`docs/architecture.md`](docs/architecture.md) | How it works: the executables, sessions, the apply model, data-folder trust, firewall, locks, status, what runs elevated |
| [`docs/release-checklist.md`](docs/release-checklist.md) | What is checked before a release is published |
| [AetherSDR `docs/COMMIT-SIGNING.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/COMMIT-SIGNING.md) | Full cross-platform signing setup (SSH and GPG) |
| [AetherSDR `docs/PR-WORKFLOW.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/PR-WORKFLOW.md) | Draft-PR conventions, stale-branch policy, recovering from a red `main` |

---

## Reviews and merging

PR review responsibility is divided into three tiers via
[`.github/CODEOWNERS`](.github/CODEOWNERS), the same tiers and teams as
AetherSDR. Self-approval is blocked by GitHub on every tier — your own PR
always needs review from someone else.

| Tier | Paths | Who can approve |
|---|---|---|
| **Source, tests & documentation (Tier 3)** | Everything not listed below — all of `src/` — plus `tests/`, `docs/`, and `resources/`, **markdown included** | `@aethersdr/reviewers` |
| **Infrastructure (Tier 2)** | `*.md` *outside* `docs/`, `resources/`, and `tests/` (`README.md`, `CHANGELOG.md`, and the AI-instruction files `AGENTS.md` / `CLAUDE.md` / `GEMINI.md` / `.github/copilot-instructions.md`), `CMakeLists.txt`, `CMakePresets.json`, `ci/`, `packaging/THIRD-PARTY-NOTICES.txt`, the routine `.github/workflows/`, `.github/dependabot.yml`, `.github/ISSUE_TEMPLATE/` | `@aethersdr/infrastructure` |
| **Maintainer-only (Tier 1)** | Governance docs (`CONSTITUTION.md` **and its canonical copy `.specify/memory/constitution.md`**, `GOVERNANCE.md`, `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, `LICENSE`), security/compliance (`SECURITY*`, `.github/CODEOWNERS`, `.github/codeql/`), and the workflows and scripts that hold release write access or feed bytes into a release artifact (`release.yml`, `qt-static.yml`, `codeql.yml`, `packaging/qt-static/`, `packaging/publish-release.py`, `packaging/sign-release.sh`) | `@aethersdr/maintainers` |

The maintainer-only tier is deliberately narrow: it covers the rules of the
project and the paths that can compromise a release. Everything that is
*documentation about how to build the thing* — including the AI-instruction
files — sits at Tier 2, so day-to-day iteration isn't maintainer-gated.

Note the ordering rule behind this, if you ever edit `.github/CODEOWNERS`:
`*.md` has no slash, so it matches markdown at **any** depth, and CODEOWNERS
resolves by *last match*, not by specificity. The `docs/`, `resources/`, and
`tests/` lines only work because they sit below the `*.md` glob — move them
above it and every `.md` under those directories quietly falls back to Tier 2.

Bot-opened PRs still require a human reviewer regardless of tier — bots are
intentionally **not** code owners.

---

## What We Will Not Accept

- **Copied proprietary code.** Clean-room implementations from documentation
  and observed device behavior are fine (Principle IV).
- **Changes that break the core path.** Test: a client connects to a port →
  bytes pass unchanged both ways → the client leaves and the serial port is
  closed.
- **Protocol translation in the bridge.** The TCP connection carries exactly
  the bytes on the serial wire. RFC 2217, telnet framing and remote DTR/RTS
  control are not goals of this bridge ([why](docs/architecture.md#raw-tcp-only));
  devices that need them should use [ser2net](docs/ser2net.md).
- **Large reformatting PRs.** Fix style only in files you're modifying.
- **UX, visual, or architecture changes without an approved RFC.** Open
  a `[RFC]` issue first — see [GOVERNANCE.md](GOVERNANCE.md).

---

## AI-Assisted Feature Requests

**You don't need to be a developer to contribute.** Describe your idea to an
AI assistant in plain English, ask it for a structured GitHub issue (What /
Why / How / the device and its serial settings), and paste the result into the
feature-request form.

### What makes a good request

- **Be specific.** "Show which client is connected in the tray icon" not "better status."
- **Describe the problem.** Tell us *why*, not just *what*.
- **Name the device.** Its manual's serial section is very helpful.
- **One feature per issue.**

---

## Notes for AI Agents

Read [`AGENTS.md`](AGENTS.md) first — it is the authoritative project context
for every AI tool.

`AGENTS.md` is documentation, not policy: where it appears to conflict with
[`CONSTITUTION.md`](CONSTITUTION.md) or [`GOVERNANCE.md`](GOVERNANCE.md),
those files win, and the conflict is a bug in `AGENTS.md` worth reporting.

The limits on what an automated agent may change without a human deciding are
defined in [`GOVERNANCE.md`](GOVERNANCE.md#ai-contributors) §AI Contributors,
which is Tier 1. `AGENTS.md` §"Autonomous Agent Boundaries" elaborates them
but may not widen them.

---

## Code of Conduct

Be respectful, constructive, and patient. Ham radio has a long tradition
of helping each other learn — bring that spirit here.

73 de KK7GWY

## License

By contributing to AetherSerialBridge, you agree that your contributions will
be licensed under the [GNU General Public License v3.0](LICENSE).
