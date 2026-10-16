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

1. Perform integration from the primary worktree after maintainer approval. Fetch the shared branch and confirm its tip has not changed unexpectedly:

   ```text
   git fetch origin dev/feature
   git log --oneline -1 origin/dev/feature
   git diff --check dev/<ticket>...origin/dev/feature
   ```

   If the shared tip changed after the review handoff, stop and coordinate before rebasing or merging. Resolve any accepted update in the temporary worktree, rerun validation, and obtain review approval again.
2. From the primary worktree, create the squash integration commit:

   ```text
   git switch dev/feature
   git pull --ff-only origin dev/feature
   git merge --squash dev/<ticket>
   git commit -m "<coherent integration message>"
   ```

   Resolve conflicts only with maintainer coordination; abort the merge and return to review if the result changes the agreed scope. Follow the repository's [branch model](branch-model.md) for promotion from `dev/feature` to `main`.
3. Verify the integrated state before cleanup. Confirm the new commit is on `dev/feature`, inspect the final diff, run the required validation, and confirm the worktree is clean:

   ```text
   git log --oneline -1 dev/feature
   git diff --check origin/dev/feature..dev/feature
   git status --short --branch
   ```
4. Remove temporary state only after verification and acceptance. Remove the worktree first, then delete its branch:

   ```text
   git worktree remove ../<repository>-<ticket>
   git branch -d dev/<ticket>
   git worktree prune
   ```

   Do not leave abandoned agent worktrees or branches behind.

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
