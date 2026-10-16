# Agent Development Workflow

This workflow applies to every agent-initiated repository change, regardless of size or file type: code, documentation, comments, CI, configuration, and maintenance edits.

## Before Implementation

1. **Discuss the change in the issue tracker.** Use a GitHub issue to record the purpose, expected scope, acceptance criteria, and relevant constraints. Do not begin implementation until the scope is agreed with the maintainer.
2. **Fetch current remote state.** From the primary worktree, run `git fetch origin` and inspect the current `origin/main` tip. The implementation base is `origin/main`, not an arbitrary local branch.
3. **Create isolated implementation state.** Create a dedicated worktree and temporary branch from `origin/main`, for example:

   ```text
   git fetch origin
   git worktree add -b dev/<ticket> ../<repository>-<ticket> origin/main
   ```

   Confirm that the temporary branch starts at the fetched `origin/main` commit before editing.
4. **Keep the primary worktree clean for shared operations.** The primary worktree is reserved for inspection, fetching, and user-directed operations. Agent implementation, generated files, and commits belong in the dedicated worktree.

## Implementation And Handoff

1. Implement only the agreed issue scope in the temporary feature branch.
2. Run the validation appropriate to the change and record the commands and results. Documentation-only changes should use document-contract and diff-scope checks; runtime or C++ tests are not required unless the issue changes runtime behavior.
3. Finish with a review handoff containing:
   - issue and agreed scope;
   - temporary branch name;
   - dedicated worktree path;
   - implementation commit(s);
   - validation commands and results;
   - known limitations or follow-up work.
4. **Implementation completion is not integration approval.** A completed branch is ready for maintainer review; it is not a pull request, merged change, published branch, or accepted issue resolution.
5. Preserve the dedicated worktree and branch until the maintainer completes review and authorizes cleanup.

## Review And Approval Gates

1. The maintainer reviews the handoff and the branch diff in the preserved worktree.
2. Do not create or ready a pull request until the maintainer explicitly approves the reviewed change. If authorized, create the pull request against `main` and include the issue and validation results.
3. The maintainer decides when the pull request is merged. Automated checks are validation evidence, not human review or merge authorization.
4. Issue closure, remote branch deletion, branch protection changes, force pushes, releases, and other repository administration each require separate explicit maintainer authorization.

## Integration And Cleanup

1. After explicit approval, verify that `origin/main` has not moved unexpectedly. If the baseline changed, update the feature branch in the preserved worktree, rerun validation, and obtain review approval again.
2. The maintainer merges the approved pull request to `main` with squash merge, producing one concentrated mainline commit for the reviewed development effort.
3. Verify the merged commit and required checks on `main` before cleanup.
4. Only after maintainer authorization and successful verification, remove the temporary worktree first, then delete its local and remote feature branch:

   ```text
   git worktree remove ../<repository>-<ticket>
   git branch -d dev/<ticket>
   git push origin --delete dev/<ticket>
   git worktree prune
   ```

   Before remote deletion, verify the branch tip and confirm that no unmerged work remains.

## External Contract Checks

The workflow is satisfied only when an external check can confirm all of the following from the document and repository state:

- issue discussion and agreed scope precede implementation;
- the implementation base is fetched `origin/main`;
- implementation is isolated from the primary worktree;
- the handoff identifies branch, worktree, commit, and validation;
- implementation completion is distinct from maintainer approval and integration;
- pull-request creation, merge, issue closure, and cleanup require the appropriate explicit maintainer authorization;
- integration uses squash merge and produces one concentrated mainline commit;
- the change remains within the agreed issue scope.
