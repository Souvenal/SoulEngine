# Branch Model and Merge Discipline

## Branch Model

Development flows through three levels of branches:

1. **`dev/xxx`** — short-lived feature branches, one per piece of work.
2. **`dev/feature`** — the permanent integration branch where finished features land and interact before release.
3. **`main`** — the stable branch. Always green; every commit on it has passed CI.

## Merge Discipline

The two integration edges use different merge strategies, and the asymmetry is deliberate:

- **`dev/xxx` → `dev/feature`: squash merge, then delete the feature branch.** A feature branch accumulates WIP commits that have no long-term value; squashing collapses them into a single coherent change on `dev/feature`. The branch does not outlive the merge, so the classic repeated-diff problem of re-merging a long-lived branch never arises.
- **`dev/feature` → `main`: merge commit, never squash.** `dev/feature` lives permanently. A merge commit keeps the two branches' histories continuous, so `dev/feature` carries on after each release with no force-push or reset dance. Squashing here would recreate the branch with divergent history every cycle.

In short: **squash where branches are disposable, merge-commit where branches are permanent.**
