# Development Process

Conventions for how this repository is developed: branching, merging, CI, and related workflow agreements.

This section is distinct from [`docs/adr/`](../adr/), which is reserved for code-design decisions. If a document records *how we work* rather than *why the code is shaped the way it is*, it belongs here.

## Documents

- [Branch Model and Merge Discipline](branch-model.md) — the `dev/xxx` → `dev/feature` → `main` flow, squash integration, and linear rebase promotion
- [Agent Development Workflow](agent-workflow.md) — issue-first scope, isolated worktrees, review handoff, approval gates, and cleanup for agent-initiated changes
