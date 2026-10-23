# AetherSerialBridge — Project Context for AI Agents

This is the canonical project guide for any AI assistant working on
AetherSerialBridge — Claude Code, OpenAI Codex, Cursor, GitHub Copilot, Gemini
Code Assist, Aider, AetherClaude (the AetherSDR project's orchestrator bot), or
any other tool. Several of those tools look for their own well-known file at a
different path (`CLAUDE.md`, `.github/copilot-instructions.md`, `GEMINI.md`);
those are thin pointers back here.

AetherSerialBridge is part of the AetherSDR project. This guide follows
[AetherSDR's `AGENTS.md`](https://github.com/aethersdr/AetherSDR/blob/main/AGENTS.md):
the same rules, with this repository's build and architecture.

**This file is documentation, not policy.** The rules that bind you live in
[`CONSTITUTION.md`](CONSTITUTION.md) and [`GOVERNANCE.md`](GOVERNANCE.md), and
they outrank everything here. Where this file appears to contradict either,
they win, and the contradiction is a defect here — fix it or open an issue.
That separation is what puts this file at CODEOWNERS Tier 2 (infrastructure)
while the Constitution and GOVERNANCE.md stay Tier 1 (maintainer-only).

One passage restates policy rather than describing practice:
§"Autonomous Agent Boundaries" below elaborates the autonomy limits that
[`GOVERNANCE.md`](GOVERNANCE.md) §AI Contributors defines. It may narrow or
illustrate them, never widen them — relaxing any of those bullets is an
amendment to GOVERNANCE.md and cannot be made in a Tier-2 PR.

## Project Goal

Put serial-only station equipment on the network for AetherSDR. The bridge
runs on the computer an amplifier, tuner or rotator is cabled to (a Windows
PC, a Linux PC, or a 64-bit Raspberry Pi), publishes each serial port as its
own raw TCP port, and moves bytes unchanged in both directions. Each port has
one client at a time, and the serial port is open only while a client is
connected. It runs unattended as a Windows service or a systemd service, and
is configured from a window, a terminal interface (TUI) or the command line.

## AI Agent Guidelines

When helping with AetherSerialBridge:
- Prefer C++20 / Qt6 idioms (std::ranges, concepts if clean, Qt signals/slots over lambdas when possible)
- Keep classes small and single-responsibility
- Use RAII everywhere (no naked new/delete)
- When suggesting code: show **diff-style** changes or full function/class if small
- Test suggestions locally if possible — the build must work on Linux (x64 and arm64) and Windows
- Flag any proposal that would break the core path: a client connects → bytes pass unchanged both ways → the
  client leaves and the serial port is closed
- If unsure about a device's behavior → ask for its manual's serial section or a capture first
- **The bytes are not ours to change.** No framing, no translation, no telnet: the TCP connection carries
  exactly the serial wire's bytes ([`docs/architecture.md`](docs/architecture.md#raw-tcp-only))
- **What runs elevated is a security boundary.** The Windows service, the elevated window, the polkit helper
  and the systemd unit trust only what [`docs/architecture.md`](docs/architecture.md) says they trust; a change
  there is an architecture change (GOVERNANCE.md §RFC Process)
- **The engine stays in the console program.** `src/engine/` links only into the headless executable; the window
  never contains it (a CMake check enforces this)
- **Every user-visible change updates the user docs in the same PR** — see
  [User documentation](#user-documentation) below.
- **Read `CONTRIBUTING.md`** for contribution policy (what we accept, who
  reviews what)
- **Sign every commit you author.** Branch protection on `main` requires
  signed commits. If the contributor has not set up commit signing yet, walk
  them through AetherSDR's
  [`docs/COMMIT-SIGNING.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/COMMIT-SIGNING.md)
  **before** you commit — the top of that file is a step-by-step AI-assistant
  algorithm covering Windows / macOS / Linux / WSL / Raspberry Pi. Default to
  SSH signing; GPG is the fallback for existing GPG workflows. Verify with
  `git log --show-signature -1` after the first commit.
- **Read the Constitution before writing or reviewing code.** Canonical
  source: `.specify/memory/constitution.md`; byte-identical mirror at
  `CONSTITUTION.md`. It is the AetherSDR Constitution with the same principle
  numbers: clean-room contributions, transmit-on-intent and boundary input
  validation apply here; the radio-client principles are marked not applicable.
  Plus the defensive engineering principles adopted from Cisco's
  [Foundry Constitution](https://github.com/CiscoDevNet/foundry-security-spec/blob/main/constitution.md)
  (Evidence Over Assertion, Surface Only What Survives, Claims Are Atomic And
  Mortal, Fixes Are Demonstrated, Sandbox By Infrastructure, Operator Outranks
  Every Agent, Persist Atomically). Commit messages cite the most-load-bearing
  principle as `Principle <N>.` at the end of the subject line.

### Issue / PR Claim Protocol — Assign Yourself

A claim is a lock on **writing to the work**, and only two things take one:

- **Implementing a fix**: writing the change for an issue, or pushing
  commits to someone else's PR.
- **Landing a PR**: the pass that remediates, approves and arms auto-merge.
  It holds the PR from its first step until the merge fires, whether or not
  it ends up pushing anything.

An AI agent about to do either MUST assign itself using GitHub's `assignees`
feature **before** starting (Principle X: Claims Are Atomic And Mortal).

**A claim announces that another project member is now involved**, so it is
never taken on your own PR. **Reviewing does not claim.** Reviews, review
comments, triage and read-only work take no assignment and check none.

1. **Before** implementing or landing, check the current assignees:
   - **Unassigned, or assigned ONLY to AetherClaude (`@aethersdr-agent`)**:
     add yourself alongside. AetherClaude's assignment is a triage signal,
     NOT a claim on active work.
   - **Already assigned to another human or AI agent** (not AetherClaude):
     leave a coordination comment instead of double-assigning, and do not
     write an overlapping fix or push to the PR.
2. **While** implementing or landing: stay assigned until the fix PR merges
   or you abandon it, or until the landing's merge fires.
3. **When you stop**, release the lock: unassign.
4. **If your work is interrupted** (token limit, context loss, model failure):
   leave a brief comment and unassign. The claim dies with the agent that held it.

```bash
gh issue edit NNNN --add-assignee @me      # or: gh pr edit NNNN --add-assignee @me
gh issue edit NNNN --remove-assignee @me   # or: gh pr edit NNNN --remove-assignee @me
gh pr view NNNN --json assignees           # check current assignees
```

### Autonomous Agent Boundaries

> **Authority: [`GOVERNANCE.md`](GOVERNANCE.md) §AI Contributors.** That
> section defines these limits and is Tier 1 (maintainer-only). What follows
> is the worked-example elaboration for agent consumption — it may narrow or
> illustrate the limits, never widen them.

AI agents may autonomously fix:
- **Bugs with clear root cause** — a missing check, a missing guard, a crash fix
- **Device compatibility** — serial settings or behavior confirmed by the device's documentation or a capture
- **Build/CI fixes** — missing dependencies, platform compat

AI agents must **NOT** autonomously change:
- **Visual design** — colors, fonts, layout, theme (user preferences ≠ project direction)
- **UX behavior** — how controls work, what clicks do, keyboard shortcuts
- **Architecture** — adding new threads, changing signal routing, new dependencies, what runs elevated, what the
  bridge trusts
- **Feature scope** — adding features beyond what the issue describes
- **Default values** — changing defaults that affect all users based on one report (a port's DTR/RTS, its bind
  address, its allowed clients)

When in doubt, the agent should implement the fix and note in the PR that
design decisions need maintainer review. The project maintainer (Jeremy/KK7GWY)
is the sole authority on visual design and UX direction.

## User documentation

The operator guides are `README.md` and `docs/*.md` (`docs/README.md` is the
index). A PR that changes what a user sees or does updates the matching page
**in the same PR**, never in a follow-up. Test, CI and internal-refactor PRs
usually don't.

- **Images** in the guides are rendered from a demo configuration, never a real
  station: `docs/images/README.md` says how (`docs-shots.yml`), and
  `tools/check_docs_images.py` (static-checks) refuses an image that carries
  anything real.
- **Links:** `tools/check_doc_links.py` checks every relative link and image in
  the guides.
- **The repository's location** is one value, `ASB_REPO` in `CMakeLists.txt`;
  `tools/set_repo.py --check` fails when a guide names another.

## C++ Style Guide

- **No `goto`** — use early returns, break, or restructure the logic
- **No raw `new`/`delete`** — use `std::unique_ptr`, `std::make_unique`, or Qt parent ownership
- **No `#define` macros for constants** — use `constexpr` or `static constexpr`
- **Braces on all control flow** — even single-line `if`/`else`/`for`/`while`
- **`auto` sparingly** — use explicit types unless the type is obvious from context
- **Naming**: classes `PascalCase`, methods/variables `camelCase`, constants `kPascalCase`, member variables `m_camelCase`
- **Platform guards**: prefer `Q_OS_WIN` / `Q_OS_LINUX` for new code; platform code lives under `src/platform/`
- **Don't remove code you didn't add** — if rebasing, ensure upstream changes are preserved. Review the diff before submitting.
- **Includes**: carry them explicitly; never rely on a transitive include (a distribution Qt newer than the pin is a
  supported headless build).
- **`vendor/aethersdr/` is AetherSDR's code**, byte-exact at a pinned commit plus listed patches. Never edit it by
  hand: `tools/sync-aethersdr.sh` regenerates it and `ci/scripts/vendor-integrity.sh` fails on any other change.

### Comments

A comment states what is true **now** and why the code has this shape: the
invariant, the constraint, the non-obvious platform fact. Aim for five lines
or fewer; a design that needs more goes in a `docs/` file the comment links to.

History does not go in code. How the bug was found, what the code used to do,
which review caught it, who reported it, and the incident narrative belong in
the commit message and PR body, where `git blame` finds them. A bare `(#NNNN)`
tag is fine; retelling the issue is not.

Don't restate the code, don't add banner dividers, and don't cite
Constitution principles in code — that tag belongs in the commit subject.

Deleting or shortening a historical comment is welcome cleanup, as long as
the guard or test it describes stays. Older comments cite `INV-<n>`, `WS-<n>`,
`D<n>` and "master §" labels from the bridge's original design documents,
which are not in this repository; the tests and guards they name are what
matters.

## Build

```bash
cmake --preset linux-dev            # Windows: cmake --preset windows-dev (MSVC developer shell)
cmake --build --preset linux-dev
ctest --preset linux-dev
```

| Preset | What |
|---|---|
| `linux-dev` | The window and the console program, with the tests, against the pinned Qt |
| `linux-headless` | The console program only (no QtGui, no QtWidgets), with the tests |
| `linux-distro-headless` | The console program against a distribution's Qt 6 (down to the headless floor) |
| `windows-dev` | The window and the console program, with the tests, MSVC |

The Qt pin (version, package revision, the GUI and headless floors) is
`cmake/qt-pin.env`; CI installs exactly that Qt through aqt
(`ci/scripts/install-qt.sh`). The window needs the Qt AetherSDR ships. Release
binaries link a static Qt built from source (`packaging/qt-static/`).

Adding a test: a QtTest under `tests/`, registered with `asb_add_test` (or
`asb_add_gui_test`) in `tests/CMakeLists.txt`; the Windows native rows, which
change the machine itself, are under `tests/win/integration/` (label
`win-integration`), and the systemd suite is `tests/systemd/` (systemd as PID 1
in Docker).

### Version and release files

The version is stated in `CMakeLists.txt` (`project(AetherSerialBridge VERSION …)`,
the only one that reaches the binary) and in `CHANGELOG.md`'s top section. A
release tag is `v<version>`; the release gates compare the program's
`--version` with the tag. `docs/release-checklist.md` is what a maintainer
signs off before a draft release is published.

### `CHANGELOG.md` is a release-prep file. Do not touch it in a feature PR.

An ordinary PR — a fix, a feature, a refactor — **must not add an entry**,
however user-visible the change is. Describe it in the PR body and the commit
message instead. At release prep, the section is written *from* those PR
bodies. Reviewers: do not ask for a `CHANGELOG.md` entry, and flag one as a
change to remove if a PR adds it.

## CI and branch protection

`ci.yml` runs on every pull request and every push to `main`, on
GitHub-hosted runners: `linux`, `no-qtgui` and `sanitizers` (ubuntu-22.04,
the glibc 2.35 floor), `containers` (the ubuntu:22.04 build container and the
systemd suite), `distro-arm` (ubuntu-24.04-arm, the distribution's Qt),
`windows` (windows-latest, with the native rows), `vendor-integrity` and
`static-checks`. Every arm64 job in every workflow runs unless the repository
variable `ASB_ARM64_RUNNERS` is `false`. `release.yml` builds and gates the
release on a `v*` tag (and as a dry run on `workflow_dispatch`), after
`qt-static.yml` restores or builds the static Qt. `tools/check_qt_pin.py`
(static-checks) enforces the workflow rules: hosted runners only, actions
pinned by SHA, the Qt pin. The required checks are whatever branch protection
on `main` lists.

## Architecture Overview

[`docs/architecture.md`](docs/architecture.md) is the full account. Key
source directories:

| Directory | What |
|---|---|
| `src/app/` | The two executables' entry points, their identity and mode dispatch |
| `src/engine/` | The bridge engine: `PortBridge`, `Session`, `Pump`, `QtSerialDevice`. Console program only |
| `src/core/` | The model, the trust logic, the `Applier` (how a configuration is applied), firewall plans, status. QtCore and QtNetwork only |
| `src/cli/` | The command line, the config editor, elevation and the Linux polkit helper |
| `src/gui/` | The window (AetherSDR's theme, the edit and apply flows). Never contains the engine |
| `src/tui/` | The FTXUI terminal interface of both executables |
| `src/platform/posix/`, `src/platform/win/` | systemd, polkit and file trust on Linux; the Windows service, firewall (WMI/CIM) and ACLs on Windows |
| `vendor/aethersdr/` | AetherSDR's own files the window reuses (theme, widgets), byte-exact upstream |

**Executables:** `AetherSerialBridge.exe` / `aether-serial-bridge-gui` (the
window) and `aether-serial-bridge[.exe]` (the console program: the service, the
command line and the TUI). Only the console program moves bytes.
