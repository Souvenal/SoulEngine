# Agent Development Workflow

This workflow applies to every agent-initiated repository change, regardless of size or file type: code, documentation, comments, CI, configuration, and maintenance edits.

## Before Implementation

1. **Discuss the change in the issue tracker.** Use a GitHub issue to record the purpose, expected scope, acceptance criteria, and relevant constraints. Do not begin implementation until the scope is agreed with the maintainer.
2. **Fetch current remote state.** From the primary worktree, run `git fetch origin` and inspect the current `origin/dev/feature` tip. The implementation base is `origin/dev/feature`, not an arbitrary local branch.
3. **Create isolated implementation state.** Create a dedicated worktree and temporary branch from `origin/dev/feature`, for example:

   ```text
   git fetch origin
   git worktree add -b dev/<ticket> ../<repository>-<ticket> origin/dev/feature
   ```

   Confirm that the temporary branch starts at the fetched `origin/dev/feature` commit before editing.
4. **Keep the primary worktree clean for shared operations.** The primary worktree is reserved for inspection, fetching, and user-directed operations. Agent implementation, generated files, and commits belong in the dedicated worktree.

## Implementation And Handoff

1. Implement only the agreed issue scope in the temporary `dev/<ticket>` branch.
2. Run the validation appropriate to the change and record the commands and results. Documentation-only changes should use document-contract and diff-scope checks; runtime or C++ tests are not required unless the issue changes runtime behavior.
3. Finish with a review handoff containing:
   - issue and agreed scope;
   - temporary branch name;
   - dedicated worktree path;
   - implementation commit(s);
   - validation commands and results;
   - known limitations or follow-up work.
4. **Implementation completion is not integration approval.** A completed branch is ready for review; it is not merged, published, or an accepted resolution of the issue.

## Review And Approval Gates

1. A maintainer reviews the handoff and the branch diff.
2. Do not create a pull request until the maintainer explicitly approves the reviewed change. If a pull request is requested, it must identify the issue and include the validation results.
3. Do not close the issue until the maintainer explicitly approves integration and the accepted change has landed in `dev/feature`. Agent completion, review readiness, and issue closure are separate states.

## Integration And Cleanup

1. Before integration, fetch `origin/dev/feature` again and rebase or merge the temporary branch as needed to account for newly accepted work. Stop and coordinate if the shared branch changed unexpectedly.
2. Integrate `dev/<ticket>` into `dev/feature` with **squash merge**, producing one coherent integration commit. Follow the repository's [branch model](branch-model.md) for promotion from `dev/feature` to `main`.
3. After integration, verify the resulting `dev/feature` state and required validation.
4. Delete the temporary branch and its dedicated worktree after the change is integrated and accepted. Do not leave abandoned agent worktrees or branches behind.

## External Contract Checks

The workflow is satisfied only when an external check can confirm all of the following from the document and repository state:

- issue discussion and agreed scope precede implementation;
- the implementation base is fetched `origin/dev/feature`;
- implementation is isolated from the primary worktree;
- the handoff identifies branch, worktree, commit, and validation;
- implementation completion is distinct from maintainer approval and integration;
- pull-request creation and issue closure require explicit maintainer approval;
- integration uses squash merge and is followed by cleanup;
- the change remains within the agreed issue scope.
