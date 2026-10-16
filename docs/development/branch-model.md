# Main-Only Branch Model

## Branch Model

Development uses short-lived feature branches created from the latest `main`. `main` is the only long-lived integration branch. There is no shared integration branch between feature work and `main`.

## Pull Request Workflow

1. Fetch the latest remote state and create a feature branch from the current `origin/main`.
2. Implement and validate the change on that feature branch, then hand the branch and local worktree to the maintainer for review.
3. After maintainer approval, open a pull request from the feature branch directly to `main`. Ordinary pull request CI reports separate macOS and Windows build checks, plus the existing `pre-commit.ci - pr` check.
4. After review and required checks pass, add the pull request to the `main` merge queue. The queue validates the exact queued combination against the latest `main` baseline through the `merge_group` workflow.
5. The maintainer completes the pull request with squash merge, producing one concentrated mainline commit for the reviewed development effort.
6. After the merge is verified on `main`, the maintainer may delete the feature branch.

A queued pull request that conflicts or fails validation can be removed or fixed without indefinitely blocking unrelated candidates. Ordinary pull request runs and merge-group runs use cancellation groups that discard obsolete work for the same candidate while keeping distinct candidates separate.

## Scope

This workflow covers contribution CI and mainline integration only. Versioning, releases, nightly builds, and publishing binary artifacts are outside its scope and remain independent decisions.
