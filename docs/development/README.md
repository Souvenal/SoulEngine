# Development Process

Conventions for how this repository is developed: branching, merging, CI, and related workflow agreements.

This section is distinct from [`docs/adr/`](../adr/), which is reserved for code-design decisions. If a document records *how we work* rather than *why the code is shaped the way it is*, it belongs here.

## Documents

- [Branch and Pull Request Model](branch-model.md) — independent changes, dependent capability stacks, direct-parent review, restacking, merge-queue validation, and squash integration
- [Development Workflow](dev-workflow.md) — issue-first scope, isolated worktrees, review handoff, approval gates, and cleanup for every repository change, human or agent
- [Agent Workflow](../agents/agent-workflow.md) — agent-only behavior: issue-context recovery, session handoff, subagent role registry, and dispatch contract
