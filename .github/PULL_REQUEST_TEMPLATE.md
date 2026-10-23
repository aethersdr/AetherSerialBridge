<!--
Thanks for contributing to AetherSerialBridge!

Before opening the PR, please:
- Read AGENTS.md if this is your first contribution (or your AI tool's
  first contribution) — it has the conventions every contributor agent
  is expected to follow.
- Read CONTRIBUTING.md for contribution policy and the review tiers.
- If you're an AI agent: claim the originating issue first by assigning
  yourself via `gh issue edit <N> --add-assignee <handle>`. Double-
  assigning alongside the @aethersdr-agent triage bot is fine.
-->

## Summary

<!-- One-paragraph description of what changes and why.  If this fixes
an issue, lead with "Fixes #N" or "Closes #N". -->

## Intent

<!-- What will be true after this merges that is not true now, in one or two
sentences, and anything deliberately left out of scope.  Review starts here:
the diff is judged against this before it is read line by line, so a correct
diff for the wrong goal is caught first. -->

## Constitution principle honored

<!-- Cite the principle by Roman numeral when relevant.  E.g.
"Principle VII — the request is validated where it enters the helper."
"Principle IV — code is clean-room, not derived from decompiled sources."
See CONSTITUTION.md for the full list. -->

## Test plan

- [ ] Local build passes (`cmake --build --preset linux-dev` or `windows-dev`)
- [ ] Behavior verified with a real serial device if applicable (which one)
- [ ] Existing tests pass (CI)
- [ ] Reproduction steps documented if user-reported bug

## Checklist

- [ ] Commits are signed (AetherSDR's `docs/COMMIT-SIGNING.md`)
- [ ] Code is clean-room — not decompiled, disassembled, or
      reverse-engineered from a proprietary binary (Principle IV)
- [ ] Bytes still pass unchanged between the client and the serial port
- [ ] Anything that runs elevated still trusts only what `docs/architecture.md` says it trusts
- [ ] Documentation updated if user-visible behavior changed — `README.md` and `docs/`.
      **Not `CHANGELOG.md`**, which is a release-prep file a PR must not add to
      (AGENTS.md); describe it in the Summary above instead
- [ ] Security-sensitive changes reference a GHSA if applicable
