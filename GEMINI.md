# Gemini Code Assist Instructions for AetherSerialBridge

**Canonical project guide: [`AGENTS.md`](AGENTS.md).** Read it before
writing or recommending code. This file holds only the must-knows that fit
in Gemini's chat context efficiently.

## Must-knows before suggesting code

1. **Signed commits required on `main`.** Branch protection enforces
   it. See AetherSDR's
   [`docs/COMMIT-SIGNING.md`](https://github.com/aethersdr/AetherSDR/blob/main/docs/COMMIT-SIGNING.md);
   the doc has explicit AI Assistant Instructions at the top — point
   yourself or the contributor at that doc for setup.

2. **The Constitution governs every contribution.** See
   [`CONSTITUTION.md`](CONSTITUTION.md): the AetherSDR Constitution with
   the same principle numbers, structured per Cisco's
   [Foundry Constitution](https://github.com/CiscoDevNet/foundry-security-spec/blob/main/constitution.md)
   spec. Commit-message format: `Short description (#NNNN). Principle <N>.`
   when the change is principle-relevant.

3. **Enforcement is by infrastructure, not prompt** (Principle XII):
   branch protection (required CI checks, CODEOWNERS review, signed
   commits) for every contributor. Don't suggest patches that try to work
   around it.

4. **The bytes are not ours to change.** The TCP connection carries exactly
   the serial wire's bytes: no framing, no telnet, no translation.

5. **What runs elevated is a security boundary** (the Windows service, the
   elevated window, the polkit helper, the systemd unit). Untrusted input is
   validated where it enters (Principle VII).

6. **Assign yourself before implementing a fix or landing a PR — not
   to review** (`gh issue edit NNNN --add-assignee @me` or
   `gh pr edit NNNN --add-assignee @me`). The assignees list is the
   visible multi-agent write lock — Principle X. Never claim your own PR.

## C++ / Qt6 style essentials (full guide in AGENTS.md)

- C++20, Qt6, no naked `new`/`delete`, no `goto`, braces on all control flow.
- Platform guards prefer `Q_OS_WIN` / `Q_OS_LINUX`; platform code lives under `src/platform/`.
- Naming: classes `PascalCase`, methods `camelCase`, member vars
  `m_camelCase`, constants `kPascalCase`.

## Multi-agent context

The AetherSDR project is touched by ≥6 distinct AI tools. PR base must be
verified current before producing patches (Principle X: Claims Are Atomic
And Mortal). Stale-snapshot overwrites that silently revert prior merged work
are the canonical multi-agent failure mode this principle exists to prevent.

For everything else, read `AGENTS.md`.
