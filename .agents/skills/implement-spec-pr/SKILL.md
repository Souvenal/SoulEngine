---
name: implement-spec-pr
description: Implement ticket stacks through reviewed PRs with explicit unmerged-branch dependencies.
disable-model-invocation: true
---

# Implement a spec PR stack

Use this PR-first procedure for a capability split into leaf tickets. Follow the [root guidance](../../../AGENTS.md), [repository workflow](../../../docs/development/agent-workflow.md), and [branch model](../../../docs/development/branch-model.md); issue and ADR contracts remain authoritative. This skill grants no additional permission for PR readiness, merges, issue closure, history rewrites, or cleanup.

Keep these boundaries throughout:

- Give every ticket its own worktree, scoped implementation, `/tdd` loop for testable behavior, `/code-review`, and review handoff.
- Integrate each leaf only through its own PR. A local merge of a leaf into a capability branch is not delivery.
- A downstream branch may use an unmerged PR head as a **code dependency**. This is neither approval nor integration; the **merge dependency** remains until the required upstream PR merges.
- Independent and code-ready leaves may proceed concurrently. Keep their branches, PRs, hashes, and states visible in the existing issue, handoff, or shared-task surfaces.

## 1. Recover the DAG and ledger the stack

1. Complete [issue-context recovery](../../../docs/development/agent-workflow.md#issue-context-recovery), including relevant PR and branch state; use the [issue tracker commands](../../../docs/agents/issue-tracker.md#recover-complete-issue-context).
2. Create the named capability branch from fetched `origin/main`, or verify the approved base and current tip of an existing one. Record exact hashes. This branch coordinates the capability; keep local leaf merges out of it.
3. Record every in-scope leaf and dependency edge in a shared ledger. For each leaf include its ticket, owner or unassigned status, worktree, branch, base branch and full commit hash, every dependency PR and full head hash, PR target, current head hash, validation/review evidence, and state.
4. Report contract contradictions before editing. Update the ledger whenever a base, head, PR, or state changes.

**Done when:** every leaf and edge has the required exact entries, concurrent owners can see them, and no unresolved contract contradiction is hidden.

## 2. Implement and review each leaf

1. Start each frontier leaf from the current capability tip or from an exact recorded unmerged dependency PR head. Before implementing a leaf with multiple unmerged dependencies, assemble every exact parent head into that leaf itself as a named disposable base assembly; record the assembly head and every parent hash as its implementation and review base. These parent-to-leaf base assemblies are code dependencies, never integration. Preserve the per-ticket dedicated-worktree rule.
2. Implement only that ticket, using `/tdd` where behavior is testable. Run appropriate validation and make the local implementation commit.
3. Run `/code-review` against the recorded implementation or base-assembly head so the review isolates the ticket-only diff. Address findings, rerun validation, and record evidence against the current head. Hand off the preserved worktree for maintainer review.

**Done when:** the diff is ticket-scoped, validation and agent review pass at the recorded head, and exact base, dependency, and head hashes are current. The leaf is `implemented-locally` after implementation and `agent-reviewed` only after review.

## 3. Maintain stacked PRs under existing gates

1. After implementation, validation, and agent review pass at the recorded head, use `/pr` and follow the workflow's default Draft publication procedure to the approved target unless explicitly local-only. Use a normal own-branch push for first publication; history rewrites remain separately gated. Record the Draft URL, exact target/base/head, dependencies, and evidence.
2. For a single-parent chain, target the first leaf at the capability branch and each dependent leaf at its immediate parent by default, so the actual PR diff is ticket-scoped. Follow an explicitly approved program-specific target when it differs. Record the implementation/review base.
3. A capability-targeted PR based on unmerged dependencies includes their upstream diffs until they merge; a multi-parent leaf likewise needs review against its recorded base assembly because one PR base cannot represent every dependency. In these topologies, do not describe the actual PR diff as ticket-only. After upstream PRs merge, retarget and restack the leaf under section 4, then re-review the actual PR diff.
4. Record actual maintainer readiness, approval, and merge events separately from local evidence. Only an authorized PR merge integrates a leaf into the capability.

**Done when:** each open PR has the spec-approved target, current head and validation evidence, a ticket-only review against its recorded implementation base, and a state supported by an actual review or merge event.

## 4. Restack a leaf without integrating it

1. When a parent advances or is squash-merged, confirm the affected worktrees are clean and record the immutable old parent tip, old leaf tip, expected new parent, expected ticket commit set, and current remote leaf tip. For a multi-parent leaf, also record the old base-assembly head. Restack children in dependency order.
2. In each leaf worktree, run `git rebase --onto <new-parent-or-base> <recorded-old-base-sha>` to replay only that leaf's commits. For a single-parent leaf, the old base is the old parent tip. For a multi-parent leaf, the old base is the old base-assembly head, not one parent's tip; rebuild the new disposable base assembly from exact new parent heads first. Do not routinely merge a parent tip into a leaf for freshness; a local merge in either direction is not integration.
3. Resolve only conflicts within the leaf's existing scope. Verify parent ancestry, the expected ticket commit set, no unexpected merge commits, and the ticket-only diff. Rerun validation and `/code-review`, then record the new parent tip and leaf head.
4. Rewriting a published leaf requires the existing explicit maintainer authorization. After approval, use an exact lease such as `git push --force-with-lease=refs/heads/<leaf>:<old-remote-sha> origin HEAD:refs/heads/<leaf>`. After a parent PR merges, retarget its direct child PR to the parent's former base and restack descendants before continuing review. A squash result is a new integrated commit; the old parent head need not become an ancestor of the base.

**Done when:** each restacked leaf has exact current hashes, a linear ticket-only diff, fresh review/validation, an authorized matching remote head when published, and no delivery state advanced by a local operation.

## 5. Keep validation-only assemblies disposable

Use a uniquely named disposable validation base/branch for combined validation outside a leaf's recorded pre-implementation base assembly. Record the disposable branch/head and every exact source hash with the acceptance results. Neither a leaf base assembly nor a validation-only assembly is integration; a validation-only branch is never a PR source or merged state.

**Done when:** results identify the disposable head and all source hashes, validation-only branches remain separate from delivery branches, and no leaf or capability state advanced because of an assembly.

## 6. Validate and deliver the capability

1. Wait until every required leaf PR is `PR-merged`. Verify that the actual capability branch contains every required leaf PR's resulting integration commit; record each integration commit, its containment check, and the exact capability tip. After squash merge, verify the new integrated commit rather than requiring ancestry from the old leaf head. Local leaf merges do not satisfy this gate.
2. Run final `/code-review` and the combined capability acceptance against that exact capability tip. Disposable assembly results are supporting evidence only.
3. After combined acceptance and final agent review pass at the recorded capability head, follow the workflow's default Draft publication procedure for the capability PR to `main` unless explicitly local-only. Maintainer readiness and merge remain separate gates. Claim `main-delivered` only after that PR actually merges and the resulting `main` commit and required checks are verified. Issue closure and cleanup remain separate repository-gated actions.

**Done when:** all required leaf PRs are merged, final capability review and combined acceptance pass on the merged baseline, and the capability either has a `draft-PR-open` PR targeting `main` or is explicitly local-only. Delivery is complete only after merge verification.

## State vocabulary

Use these exact labels in ledgers and handoffs. Apply the PR states to each leaf and to the capability's own PR; do not infer a later state from earlier evidence.

| State | Meaning |
| --- | --- |
| `implemented-locally` | The scoped diff is committed in its dedicated worktree with appropriate local validation; no review, PR, approval, or integration is implied. |
| `agent-reviewed` | Agent review is complete at the recorded head and findings are addressed; subagent review is not maintainer approval. |
| `draft-PR-open` | An agent-reviewed Draft PR exists at the recorded target/base/head with evidence; it is not Ready, approved, merged, or integrated. |
| `PR-open` | The PR is non-draft/Ready after explicit maintainer readiness approval at the recorded head; merge remains separate. |
| `maintainer-approved` | The required maintainer approval is explicit for the current change; the PR is not thereby merged. |
| `PR-merged` | The named PR is actually merged into its recorded target; record the resulting integration commit. |
| `capability-validated` | The capability tip contains every required leaf PR's resulting integration commit, and final capability review and combined acceptance pass at that exact tip; main delivery has not necessarily occurred. |
| `main-delivered` | The capability PR is merged to `main` and the resulting mainline commit and required checks are verified; closure and cleanup remain separate. |
