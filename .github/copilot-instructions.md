# GitHub Copilot Instructions for AetherSerialBridge

**Canonical project guide: [`/AGENTS.md`](../AGENTS.md).** Read it
before suggesting non-trivial code. This file holds only the highest-
priority must-knows that fit in Copilot's chat context window.

## Must-knows before suggesting code

1. **Signed commits required on `main`.** Branch protection enforces
   it. See AetherSDR's `docs/COMMIT-SIGNING.md` for setup; the doc has
   explicit AI Assistant Instructions at the top.

2. **The Constitution governs every contribution.** See
   `CONSTITUTION.md` (canonical: `.specify/memory/constitution.md`): the
   AetherSDR Constitution with the same principle numbers. Commit-message
   format: `Short description (#NNNN). Principle <N>.` when the change is
   principle-relevant.

3. **Enforcement is by infrastructure, not prompt** (Principle XII):
   branch protection (required CI checks, CODEOWNERS review, signed
   commits). Don't suggest patches that try to work around it.

4. **The bytes are not ours to change.** The TCP connection carries exactly
   the serial wire's bytes: no framing, no telnet, no translation.

5. **The engine stays in the console program.** `src/engine/` never links
   into the window.

6. **Assign yourself before implementing a fix or landing a PR — not to
   review** (Principle X). Never claim your own PR.

For everything else, read `AGENTS.md`.
