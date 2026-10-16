# Main-Only Branch Model

## Branch Model

Development uses short-lived feature branches created from the latest `main`. `main` is the only long-lived integration branch. The retired integration branch is not part of the workflow.

## Pull Request Workflow

<<<<<<< HEAD
1. Update local refs and create a feature branch from the current `origin/main`.
2. Open a pull request from the feature branch directly to `main`. Ordinary pull request CI reports separate macOS and Windows build checks; the existing `pre-commit.ci - pr` check remains an independent required gate.
3. After review and ordinary checks pass, add the pull request to the `main` merge queue. The queue processes candidates in order and creates a temporary merge group containing the candidate and preceding queued changes. The `merge_group` workflow validates that exact combination against the latest `main` baseline.
4. The queue merges an accepted pull request with squash merge, preserving one coherent commit on `main`.
5. Delete the feature branch after the pull request merges.
=======
## Promotion Workflow
>>>>>>> 2ce06b7 (docs: clarify promotion branch safety (#43))

A queued pull request that conflicts or fails validation can be removed or fixed without indefinitely blocking unrelated candidates. A new pull request run and its merge-group run cancel obsolete work for the same candidate, while distinct pull requests and branches retain separate concurrency groups.

<<<<<<< HEAD
## Scope

This workflow covers contribution CI and mainline integration only. Versioning, releases, nightly builds, and publishing binary artifacts are outside its scope and remain independent decisions.
=======
1. **`dev/xxx` → `dev/feature`: Squash and merge, then delete the feature branch.** A feature branch accumulates WIP commits that have no long-term value; squashing collapses them into a single coherent change on `dev/feature`. The branch does not outlive the merge, so the classic repeated-diff problem of re-merging a short-lived branch never arises.
2. **`dev/feature` → `main`: Rebase and merge, never Squash and merge or Create a merge commit.** `main` receives changes only through `dev/feature`. Before merging, confirm that CI passes and that the promotion PR is up to date. If GitHub marks the PR out of date, choose **Update with rebase**. This updates the PR branch onto the current `main` tip; it does not merge the PR. After that update completes and CI passes, choose **Rebase and merge** to promote `dev/feature` while preserving one linear history.

### Post-Promotion Synchronization

After the promotion PR lands, synchronize the shared integration branch with the new `main` tip:

1. Fetch `origin/dev/feature`, record its tip, and compare it with the expected shared-branch state. If it contains unexpected commits from another contributor, stop and coordinate before rebasing.
2. Rebase `dev/feature` onto the new `main` tip locally.
3. Immediately before updating the shared branch, fetch `origin/dev/feature` again and confirm that its tip is still the one checked in step 1. If it changed, stop rather than overwrite the new remote work.
4. Update the shared branch with **`git push --force-with-lease origin dev/feature`**. The lease protects unexpected remote work from being overwritten; do not use an unconditional force push.
5. Wait for required CI after the branch update and confirm that the promoted commit remains verified.

**Update with rebase** makes an out-of-date promotion PR current before merging. **Rebase and merge** is the final merge action that promotes `dev/feature` to `main`; both operations may be required for an out-of-date PR.

This workflow keeps the permanent branches on one linear history: squash disposable work into `dev/feature`, then rebase-promote `dev/feature` to `main` and resynchronize it after promotion.
>>>>>>> 2ce06b7 (docs: clarify promotion branch safety (#43))
