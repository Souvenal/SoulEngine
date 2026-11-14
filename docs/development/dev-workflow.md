# Development Workflow

This workflow applies to every repository change, by human or agent, regardless of size or file type: code, documentation, comments, CI, configuration, and maintenance edits. Agent-only behavior — issue-context recovery, session handoff, and subagent delegation — is owned by [agent-workflow.md](../agents/agent-workflow.md).

## Before Implementation

1. **Discuss the change in the issue tracker.** Complete [issue-context recovery](../agents/agent-workflow.md#issue-context-recovery), then use a GitHub issue to record the purpose, expected scope, acceptance criteria, and relevant constraints. Do not begin implementation until the scope is agreed with the maintainer.
2. **Fetch current remote state.** From the primary worktree, run `git fetch origin`. For an independent change, inspect and use the current `origin/main` tip. For a dependent leaf, verify the exact recorded parent or base-assembly heads under the stack contract. Never infer the implementation base from an arbitrary local branch.
