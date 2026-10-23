# Claude Code Project Guide for AetherSerialBridge

> **Claude Code: read [`AGENTS.md`](AGENTS.md) for the full project guide.**
> This file exists only because Claude Code auto-loads `CLAUDE.md`.
> Everything project-wide — architecture, conventions, the constitution,
> commit signing, branch protection rules, the must-knows — lives in
> `AGENTS.md` so every AI tool reads the same source of truth.

## Why this is a thin file

The AetherSDR project's contribution surface is multi-agent in practice
(six+ distinct AI tools touch its code). Each tool has its own well-known
file at a different path:

| Tool | File |
|---|---|
| Claude Code | `CLAUDE.md` (you are here) |
| OpenAI Codex CLI / spec-kit / Foundry | `AGENTS.md` (the canonical) |
| GitHub Copilot | `.github/copilot-instructions.md` |
| Gemini Code Assist | `GEMINI.md` |

All of those are pointers to `AGENTS.md`. The duplication-by-pointer
pattern keeps each tool reading from its native discovery path without
forcing a single canonical file to be copied N times.

Read `AGENTS.md` for everything else.
