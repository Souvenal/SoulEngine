# Branch Model

## Branch Roles

`main` is the only long-lived branch. Ordinary feature branches start at fetched `origin/main` and propose a direct PR to `main`. A root spec that explicitly approves stacked delivery may instead assign a temporary integration branch to each first-level capability. That integration branch starts at a recorded fetched `origin/main` commit; its leaf branches start at its current tip. This is scoped delivery state, not a permanent develop branch.

[#123](https://github.com/Souvenal/SoulEngine/issues/123) approves this topology for the Editor control-plane program. [#125](https://github.com/Souvenal/SoulEngine/issues/125) names its canonical integration branch `dev/125-mcp-control-plane`: leaf PRs target that branch; its completed capability PR targets `main`. Before creating a stack, record the actual branch name and fetched-main base in the capability handoff. If an existing local integration branch uses a different name, record that mapping and its intended PR target explicitly; branch names do not prove ancestry or approval.

## Pull Request Workflow

1. Recover the issue and parent contracts and choose direct or explicitly approved stacked delivery. Fetch remote state and record the `origin/main` base. For a stack, create the capability integration worktree from that base, then each leaf worktree from the integration tip. Confirm ancestry before editing.
2. Implement and validate the feature or leaf scope in its own worktree. Before handing off a leaf, merge the current integration tip into the leaf and rerun affected validation and review. This parent-to-leaf freshness merge is not integration or approval. Coordinate shared integration updates; independent agents do not race branch/worktree creation or rewrite each other’s history.
3. Hand off branch, worktree, commits, recorded base, intended PR target, and checks for maintainer review. Implementation completion grants no PR or merge approval. After explicit approval of the reviewed change, open the PR to `main` for direct work, or to the recorded capability integration branch for a leaf.
4. A leaf integrates only through its maintainer-approved PR into the capability branch. Agents do not self-merge leaf branches into the capability. Validate the combined capability after each authorized leaf PR merge. When all required leaf PRs have merged, synchronize the integration branch with fetched `origin/main`, rerun combined validation, and hand off the complete diff for explicit review approval before opening/readying its integration PR to `main`.
5. The `main` PR must pass the required review and checks, then enter the `main` merge queue. The queue validates the exact queued combination against the latest `main` baseline through the `merge_group` workflow. The maintainer completes squash merge, producing one concentrated mainline commit for the reviewed feature or capability.
6. Verify the merged result and checks on `main`. Issue closure, worktree cleanup, and local/remote branch deletion retain the separate authorization gates in [Agent Development Workflow](agent-workflow.md#review-and-approval-gates); a leaf PR merge into the capability is not evidence of mainline delivery.

Ordinary `main` PR CI reports separate macOS and Windows build checks plus `pre-commit.ci - pr`. A leaf PR must run its appropriate local checks and required checks for its actual target. Do not assume `main`-filtered PR or merge-queue workflows execute on a capability target: inspect CI configuration and record any coverage gap. Final capability validation and the `main` merge queue remain required. A queued PR that conflicts or fails validation can be removed or fixed without indefinitely blocking unrelated candidates; obsolete runs may be cancelled without cancelling distinct candidates.

## Scope

This workflow covers contribution CI and mainline integration. It preserves issue-first scope, isolated worktrees, explicit maintainer review/PR/merge/closure/cleanup gates, and squash integration to `main`. Versioning, releases, nightly builds, and binary publishing remain independent decisions.
