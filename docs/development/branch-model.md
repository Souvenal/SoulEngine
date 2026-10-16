# Main-Only Branch Model

## Branch Model

Development uses short-lived feature branches created from the latest `main`. `main` is the only long-lived integration branch. The retired `dev/feature` branch is not part of the workflow.

## Pull Request Workflow

1. Update local refs and create a feature branch from the current `origin/main`.
2. Open a pull request from the feature branch directly to `main`. Ordinary pull request CI reports separate macOS and Windows build checks; the existing `pre-commit.ci - pr` check remains an independent required gate.
3. After review and ordinary checks pass, add the pull request to the `main` merge queue. The queue processes candidates in order and creates a temporary merge group containing the candidate and preceding queued changes. The `merge_group` workflow validates that exact combination against the latest `main` baseline.
4. The queue merges an accepted pull request with squash merge, preserving one coherent commit on `main`.
5. Delete the feature branch after the pull request merges.

A queued pull request that conflicts or fails validation can be removed or fixed without indefinitely blocking unrelated candidates. A new pull request run and its merge-group run cancel obsolete work for the same candidate, while distinct pull requests and branches retain separate concurrency groups.

## Scope

This workflow covers contribution CI and mainline integration only. Versioning, releases, nightly builds, and publishing binary artifacts are outside its scope and remain independent decisions.
