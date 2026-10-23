# AetherSerialBridge Governance

AetherSerialBridge is part of the AetherSDR project and is governed the same way:
this document is [AetherSDR's governance](https://github.com/aethersdr/AetherSDR/blob/main/GOVERNANCE.md)
applied to this repository. It describes how AetherSerialBridge is governed — who has authority over
what, how decisions are made, and how contributors can earn expanded roles.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the mechanics of submitting code.

---

## Project Direction

AetherSerialBridge's goal is to put serial-only station equipment on the
network for AetherSDR: each serial port becomes a raw TCP port, bytes pass
unchanged in both directions, and the bridge runs unattended as a service on
the computer the equipment is cabled to. Every technical and design decision
should be evaluated against that goal.

**Cross-platform first.** Platform-specific code is acceptable where it solves
a real problem (a Windows service, a systemd unit, polkit), but it must be
isolated (platform guards, separate files under `src/platform/`) and cannot be
the primary motivation for touching shared code.

**The project maintainer (Jeremy Fielder / KK7GWY) holds final authority on:**

- Visual design — colors, fonts, layout, theme
- UX behavior — how controls work, what clicks and shortcuts do by default
- Architecture — threading model, signal routing, new dependencies
- Feature scope — what is and isn't in scope for the project

This is not a committee. When there is disagreement about direction, the
maintainer's decision is final.

---

## Roles

### Contributor

Anyone who opens a PR. No special permissions. PRs are reviewed by the
maintainer or a Domain Maintainer with authority over the affected area.

### Triager

Can label, close, and comment on issues. Cannot merge PRs.

**How to earn it:** Demonstrated pattern of helpful, accurate issue responses.
Nominate yourself or ask in an issue — the maintainer will grant it.

### Domain Maintainer

Can review and merge PRs **within their designated area** without waiting for
maintainer review, subject to the RFC and CODEOWNERS requirements below.

Current domain areas:

| Area | Path(s) | Notes |
|------|---------|-------|
| Documentation | `docs/`, `*.md` | Operator guides, the docs images. Spans two CODEOWNERS tiers: `docs/` is Tier 3; bare `*.md` is Tier 2 |
| Build / CI | `CMakeLists.txt`, `CMakePresets.json`, `cmake/`, `ci/`, `.github/` | Build system, CI pipelines |
| Platform: Linux | `src/platform/posix/`, `packaging/linux/` | Linux-specific code only |
| Platform: Windows | `src/platform/win/`, `packaging/windows/` | Windows-specific code only |

Domain Maintainers cannot merge PRs that touch files outside their area.
Changes that touch `src/gui/`, `src/core/`, or `src/engine/` always require
maintainer review regardless of domain.

**How to earn it:** Three or more merged PRs in the area, demonstrated
understanding of the cross-platform requirements, and explicit agreement to
the project direction in this document. Open an issue titled
`[Governance] Domain Maintainer request: <area>` to start the conversation.

### Core Maintainer

Can review and merge PRs across most of the codebase, excluding areas
protected by CODEOWNERS. Expected to understand the full architecture: the
bridge engine, the apply model and what runs elevated.

This role does not currently exist outside the project maintainer. It will be
established when the project has contributors who have demonstrated sustained,
high-quality work across multiple areas over an extended period.

### Project Maintainer

Jeremy Fielder ([@ten9876](https://github.com/ten9876)). Final authority on
all decisions. Assisted by Claude (AI development partner) for implementation,
review, and issue triage — see the AI Contributors section below.

---

## RFC Process

Some changes require a written proposal — an RFC (Request for Comments) — to
be approved before implementation begins. This prevents the "implement first,
discuss never" pattern and ensures cross-platform implications are considered
up front.

### What requires an RFC

- Any change to **visual design** — colors, fonts, spacing, theme, icons
  (the window follows AetherSDR's theme)
- Any change to **default UX behavior** — what a click, shortcut, or gesture
  does out of the box
- **New default keyboard bindings**
- **New external dependencies** (libraries, frameworks, system packages)
- **Architecture changes** — new threads, new signal routing patterns,
  changes to the apply model, to what runs elevated, or to what the bridge
  trusts
- **Platform-specific native integration** that touches shared code
- **New feature areas** substantially beyond the current scope

When in doubt, open an RFC issue first and ask.

### What does NOT require an RFC

- Bug fixes with a clear root cause
- Device compatibility fixes (serial settings, documented device behavior)
- New shortcuts that are unassigned by default and additive
- Documentation additions and corrections
- Build / CI fixes
- New applets or dialogs that don't change existing UX
- Performance improvements that don't change behavior

### How to write an RFC

Open a GitHub issue with the label `rfc` and the title prefix `[RFC]`.
Describe:

1. **Problem** — what is broken or missing
2. **Proposal** — what you want to change
3. **Cross-platform impact** — how this affects Linux (x64 and Raspberry Pi) and Windows
4. **Alternatives considered** — what else you looked at

The maintainer will comment with approval, rejection, or requested changes.
Do not open a PR until the RFC issue is approved.

---

## CODEOWNERS

PR review is gated by [`.github/CODEOWNERS`](.github/CODEOWNERS), which is the
authoritative source of who must approve what (last-match-wins). It defines
three tiers, broadest → most restrictive:

- **Tier 3 — source code, tests, and documentation** (`@aethersdr/reviewers`):
  all of `src/`, plus `tests/`, `docs/`, and `resources/`, markdown included,
  plus anything not enumerated below. The broad reviewer roster.
- **Tier 2 — infrastructure** (`@aethersdr/infrastructure`): `*.md` outside
  those directories (`README.md`, `CHANGELOG.md`, …), `CMakeLists.txt`,
  `CMakePresets.json`, the routine CI workflows under `.github/workflows/`, the
  CI containers and scripts under `ci/`, and the AI-instruction files (`AGENTS.md`, `CLAUDE.md`,
  `GEMINI.md`, `.github/copilot-instructions.md`).
- **Tier 1 — governance / security** (`@aethersdr/maintainers`): the governance
  docs (the Constitution — **both** its canonical copy at
  `.specify/memory/constitution.md` and the root `CONSTITUTION.md` mirror —
  plus `GOVERNANCE.md`, `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, `LICENSE`),
  the security/compliance paths (`SECURITY*`, `.github/CODEOWNERS` itself, the
  CodeQL config), and the workflows and scripts that hold release write access
  or feed bytes into a release artifact (`release.yml`, `qt-static.yml`,
  `codeql.yml`, `packaging/qt-static/`, `packaging/publish-release.py`,
  `packaging/sign-release.sh`).

The lists above are a summary. `.github/CODEOWNERS` is the enforced artifact
and wins on any disagreement; the path-level breakdown for contributors is the
table in [`CONTRIBUTING.md`](CONTRIBUTING.md#reviews-and-merging). Deliberately
not restated here: the exact workflow filenames, which change as workflows are
added and renamed.

One subtlety worth knowing before editing `.github/CODEOWNERS`: a pattern with
no slash (`*.md`, `CMakeLists.txt`) — **or with only a trailing one**
(`docs/`, `tests/`) — matches at **any depth**. Only a *leading* slash
(`/docs/`) anchors a pattern to the repository root.

The `*.md` glob reaches markdown inside every directory, so the `docs/`,
`resources/`, and `tests/` lines have to come *after* it in the file for those
directories' markdown to stay at Tier 3 — CODEOWNERS resolves by last match,
not by specificity.

Tier 1 is deliberately narrow: a path belongs there only if a wrong change to
it would alter **who decides things** or **what gets signed**. Documentation
that merely describes how to build the software — including the AI-instruction
files, which carry architecture, build steps, and style conventions rather than
policy — sits at Tier 2. Where an instruction file appears to conflict with
this document or the Constitution, those win; the conflict is a defect in the
instruction file, not a governance change, so gating it at Tier 1 bought
friction rather than control.

This does not touch **project direction**, which §Project Direction reserves
to the Project Maintainer regardless of who can approve an edit.

Self-approval is blocked by GitHub on every tier — your own PR always needs a
review from someone else. The Tier-1 paths are hard gates: no merge without
maintainer sign-off (an admin override is the only bypass). Team rosters live
in the GitHub org; `.github/CODEOWNERS` carries the exact patterns.

---

## AI Contributors

The AetherSDR project has two categories of AI involvement:

### Automated agents (AetherClaude and others)

AetherClaude is the AetherSDR project's official automated contributor: it
monitors the issue tracker and opens PRs for issues labeled `aetherclaude`.

**The following limits are normative and defined here.** They apply to every
automated agent that opens PRs against this repository, not only AetherClaude:

- **May autonomously fix:** bugs with a clear root cause, device
  compatibility issues confirmed against the device's documentation or a
  capture, build/CI failures.
- **May not autonomously change:** visual design (colours, fonts, layout,
  theme), UX behaviour (what controls do, keyboard shortcuts), architecture
  (new threads, signal routing, new dependencies), feature scope beyond what
  the issue describes, or default values affecting all users.

When in doubt, the agent implements the fix and notes in the PR that a design
decision needs maintainer review. The Project Maintainer is the sole authority
on visual design and UX direction.

`AGENTS.md` §"Autonomous Agent Boundaries" elaborates these limits with
worked examples for agent consumption. That file is Tier 2 and **may not
widen them** — where it and this section differ, this section governs, and
the difference is a defect in `AGENTS.md`. Widening an automated agent's
autonomy is an amendment to this document and follows §Amendments.

Bot-opened PRs are reviewed under the same CODEOWNERS tiers as any other PR.
@AetherClaude is deliberately **not** a member of any code-owner team, so a
bot PR always requires approval from a human code owner of every tier it
touches, and the bot can never approve its own work or another agent's.

### AI-assisted human contributions

Contributions generated with AI tools (GPT, Claude, Copilot, etc.) are
welcome and held to **the same standards as any other PR**. The human
submitting the PR is responsible for understanding the change, testing it,
and ensuring it meets the project's guidelines. "Generated by AI" is not a
reason to relax review standards — if anything, it warrants closer scrutiny
of cross-platform correctness and of anything that runs elevated.

AI-assisted PRs that touch protected areas still require RFC approval first.

---

## Decision Making

For ordinary PRs, the process is simple: a Triager or Domain Maintainer can
comment, the maintainer reviews and merges or requests changes.

For significant decisions (RFC-required changes, new maintainer roles,
changes to this document), the maintainer may open a discussion period of
at least 5 days before deciding. Community input is welcome and considered,
but the maintainer's decision is final.

---

## Amendments

This document may be updated by the project maintainer at any time. Significant
changes will be announced in the GitHub Discussions or via a PR with the label
`governance` so the community can comment before the change lands.

---

*73 de KK7GWY*
