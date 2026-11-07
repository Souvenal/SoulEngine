# Branch and Pull Request Model

## Independent Changes

An independent change can be built, tested, reviewed, and merged from the latest `main` without another unmerged change. Separate issues or disjoint files alone do not make changes independent.

Independent development uses short-lived feature branches created from the latest `main`. `main` is the only permanent integration branch. A maintainer-approved capability may use a temporary integration branch and dependent leaf stack as described below.

## Independent Pull Request Workflow

1. Fetch the latest remote state and create a feature branch from the current `origin/main`.
2. Implement and validate the change on that feature branch, then hand the branch and local worktree to the maintainer for review.
3. After maintainer approval, open a pull request from the feature branch directly to `main`. Ordinary pull request CI reports separate macOS and Windows build checks, plus the existing `pre-commit.ci - pr` check.
4. After review and required checks pass, add the pull request to the `main` merge queue. The queue validates the exact queued combination against the latest `main` baseline through the `merge_group` workflow.
5. The maintainer completes the pull request with squash merge, producing one concentrated mainline commit for the reviewed development effort.
6. After the merge is verified on `main`, the maintainer may delete the feature branch.

A queued pull request that conflicts or fails validation can be removed or fixed without indefinitely blocking unrelated candidates. Ordinary pull request runs and merge-group runs use cancellation groups that discard obsolete work for the same candidate while keeping distinct candidates separate.

## Dependent Capability Stacks

Use a stack when a leaf requires unmerged parent code or an earlier leaf's contract. A capability issue must identify the dependency edges and temporary integration target. Each leaf keeps its own issue scope, dedicated worktree, validation, and review.

### Stack And Review Topology

1. Start each leaf from the exact recorded parent head. Record the branch, worktree, parent branch and full hash, leaf head, PR target, validation, and review state in a shared ledger.
2. For a single-parent chain, target the first leaf PR at the capability branch and each dependent leaf PR at its immediate parent by default. This makes the actual GitHub PR diff ticket-scoped while preserving the dependency chain. A maintainer may approve a different target for a program-specific contract, but a capability-targeted leaf with unmerged dependencies has a cumulative PR diff. A multi-parent leaf requires review against its recorded base assembly because one PR base cannot represent every dependency.
3. Integrate each leaf only through its own merged PR. Using a parent as a code dependency or merging locally into a capability branch is neither review approval nor integration.
4. Keep each leaf's delivered history linear, with `parent..leaf` limited to that leaf's commits. Do not routinely merge a parent tip into a leaf merely to refresh it.

### Restack After A Parent Changes

1. Before rewriting anything, confirm every affected worktree is clean and record the immutable old parent tip, old leaf tip, expected new parent, expected ticket commit set, and current remote leaf tip. For a multi-parent leaf, also record the old base-assembly head.
2. After a parent advances or is squash-merged, restack children in dependency order. In each child worktree, replay only that child's commits with:
   ```text
   git rebase --onto <new-parent-or-base> <recorded-old-base-sha>
   ```
   For a single-parent leaf, the old base is the old parent tip. For a multi-parent leaf, the old base is the old base-assembly head, not one parent's tip; first rebuild the new disposable base assembly from the exact new parent heads, then replay the leaf commits onto it.
3. Resolve only conflicts belonging to the leaf's existing scope. Verify parent ancestry, the expected ticket commit set, the absence of unexpected merge commits, and the ticket-only diff; rerun validation and review at the new head. Code tests alone do not validate topology.
4. Rewriting a published branch requires the existing explicit maintainer authorization. After approval, publish each rewritten leaf with an exact lease, for example:
   ```text
   git push --force-with-lease=refs/heads/<leaf>:<old-remote-sha> origin HEAD:refs/heads/<leaf>
   ```
   This stack rule grants no force-push, merge, closure, or cleanup permission by itself.
5. After a parent PR merges, retarget its direct child PR to the parent's former base and restack the remaining descendants before continuing review. A squash merge creates a new integrated commit identity; do not expect the old parent head to become an ancestor of the base.
6. Update the shared ledger and PR descriptions with the new base, head, validation, and review evidence.

## Scope

This workflow covers contribution CI, branch topology, review, and integration. Versioning, releases, nightly builds, and publishing binary artifacts are outside its scope and remain independent decisions.
