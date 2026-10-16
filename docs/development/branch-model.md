# Branch Model and Merge Discipline

## Branch Model

Development flows through three levels of branches:

1. **`dev/xxx`** — short-lived feature branches, one per piece of work.
2. **`dev/feature`** — the permanent integration branch where finished features land and interact before release.
3. **`main`** — the stable branch. Always green; every commit on it has passed CI.

## Promotion Workflow

The two integration edges use different merge strategies, and the asymmetry is deliberate:

1. **`dev/xxx` → `dev/feature`: Squash and merge, then delete the feature branch.** A feature branch accumulates WIP commits that have no long-term value; squashing collapses them into a single coherent change on `dev/feature`. The branch does not outlive the merge, so the classic repeated-diff problem of re-merging a short-lived branch never arises.
2. **`dev/feature` → `main`: Rebase and merge, never Squash and merge or Create a merge commit.** `main` receives changes only through `dev/feature`. Before merging, confirm that CI passes and that the promotion PR is up to date. If GitHub marks the PR out of date, choose **Update with rebase**. This updates the PR branch onto the current `main` tip; it does not merge the PR. After that update completes and CI passes, choose **Rebase and merge** to promote `dev/feature` while preserving one linear history.

### Post-Promotion Synchronization

After the promotion PR lands, synchronize the shared integration branch with the new `main` tip:

1. Check the remote state and confirm that `origin/dev/feature` has no unexpected commits from another contributor.
2. Rebase `dev/feature` onto the new `main` tip locally.
3. Update the shared branch with **`git push --force-with-lease origin dev/feature`**. The lease protects unexpected remote work from being overwritten; do not use an unconditional force push.
4. Wait for required CI after the branch update and confirm that the promoted commit remains verified.

**Update with rebase** makes an out-of-date promotion PR current before merging. **Rebase and merge** is the final merge action that promotes `dev/feature` to `main`. The two operations are separate and may both be required for an out-of-date PR.

This workflow keeps the permanent branches on one linear history: squash disposable work into `dev/feature`, then rebase-promote `dev/feature` to `main` and resynchronize it after promotion.
