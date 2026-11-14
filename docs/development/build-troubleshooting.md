# Build Troubleshooting

Verified failure signatures and repairs for the SoulEngine build. Read this before diagnosing any environment, dependency, or configuration failure — most of these have known one-command repairs. Agents should also search their project memory (OpenViking experiences/preferences) before running diagnostic loops; this doc records the same knowledge in repo-visible form.

## Generic configuration failure: `error: <no error object>`

**Signature.** `xmake` fails configuration or package resolution with a bare `error: <no error object>` plus a note like "the following packages were not found in any repository" naming packages that ARE defined in the project (for example `imgui-club`), or downstream targets report `ld: library 'vulkan-headers' not found` for packages that installed fine before.

**Cause.** xmake lost its package-cache/configuration state: after `xmake clean --all`, a wiped `.xmake/` directory, or a package-repo sync, the local package index no longer resolves project-defined or repo packages.

**Repair.**

```bash
xmake repo -u   # resync package repos (required when .xmake was deleted)
xmake f -y      # reconfigure
xmake -y        # build
```

Do NOT keep digging through linker logs while configuration is broken — fix configuration first; the link errors disappear with it.

## Module-BMI staleness: "definition of module 'X' is not available"

**Signature.** An implementation unit (`*.cpp`) cannot find the BMI of a module it imports, or a newly added `import` in an interface unit is not visible to consumers, even though the module sources are correct.

**Cause.** xmake's cached module dependency scan predates the import change.

**Repair (escalating, stop at the first one that works).**

1. Rebuild the affected targets narrowly: `xmake -y <TargetA> <TargetB>`.
2. Regenerate the scan of the owning module: `xmake -y -r <Module>`.
3. Reconfigure: `xmake f -c -y`, then rebuild.

## `xmake clean --all` is not a troubleshooting tool

`xmake clean --all` deletes generated build artifacts AND disturbs package-cache state; on this project it repeatedly produced the generic configuration failure above, which then blocked unrelated work for long spans. Do not reach for it. The escalation ladder in the previous section handles every staleness case it was historically invoked for. If you believe nothing short of a full wipe can help, delete the project-generated `build/` directory (after verifying its resolved path is inside the workspace) and run `xmake repo -u` before reconfiguring — that restores package state too.

## Missing system packages

`vulkan-headers`, `volk`, `slang`, `tracy`, and `imgui-club` must be present for full-tree configuration. When one is missing, configuration fails before any compilation — the error is a package-resolution error, not a source error. Check the failure message for the package name before suspecting code.
