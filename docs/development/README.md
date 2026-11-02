# Development Process

Development workflows and versioned implementation contracts for this repository.

This section is distinct from [`docs/adr/`](../adr/), which is reserved for code-design decisions. Workflow procedures and detailed implementation contracts belong here; ADRs retain architectural decisions and rationale.

## Documents

- [Editor Command Contract](editor-commands.md) — read when implementing shared human/Agent command registration, input/MCP dispatch, completion, phases, or provider lifetime
- [Branch Model](branch-model.md) — direct feature PRs and approved capability/leaf stacks, merge-queue validation, and squash mainline integration
- [Agent Development Workflow](agent-workflow.md) — issue-first scope, isolated worktrees, review handoff, approval gates, and cleanup for agent-initiated changes
