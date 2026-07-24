---
name: soulengine-build
description: Diagnose, fix, and document SoulEngine build, configuration, package, compiler, linker, module, and test failures on any supported platform or toolchain. Use when working with Xmake, C++ compilation, C++23 modules, `import std`, third-party packages, linker/runtime mismatches, test executables, or when adding reusable project-specific build knowledge after resolving an issue.
---

# SoulEngine Build

Use this skill for every project build failure and update it after a verified, reusable diagnosis. Use `coding-spec` as well whenever a repair changes project source or Xmake files.

## Build loop

1. Inspect the current root `xmake.lua` and the configured platform/toolchain; do not rely on stale comments or historical configuration.
2. Reproduce from clean generated artifacts:

   ```powershell
   xmake clean --all -y
   xmake f -m debug -y
   xmake -y
   ```

3. Treat the first compiler or linker error as the active failure. Preserve the command and diagnostic before changing policies, toolchains, scanners, or source.
4. Make the narrowest repair that explains the failure. Reconfigure and rebuild after changes to module interfaces, compiler/toolchain selection, package configuration, or runtime library settings.
5. Run tests through Xmake so target `runenvs` are applied:

   ```powershell
   xmake test -j1 -v
   ```

   Use `xmake run` only when the executable does not require test-specific environment setup.

## Diagnose by platform and toolchain

- Load the matching reference before applying a platform-specific fix.
- Current references:
  - Windows MSVC, Xmake, named modules, `import std`, CRT/linker, and test-entry diagnostics: `references/windows-msvc-modules.md`
  - Windows Xmake package fetch/build/resolution failures (xmake-repo and souvenal-repo recipes, ccache launcher interactions, pinned-version resolution): `references/windows-xmake-packages.md`
- Add a new one-level-deep reference when a resolved issue is platform-, compiler-, package-, or subsystem-specific. Link it in this list and keep `SKILL.md` limited to cross-platform workflow.

## Knowledge-capture rule

After a build issue is fixed and verified:

1. Record the exact diagnostic signature, root cause, smallest verified repair, and tempting but incorrect repairs in the relevant reference.
2. Add a new reference instead of expanding an unrelated one.
3. Record only stable project knowledge; do not add machine-specific absolute paths, transient cache hashes, or raw full logs.
4. Include the verification command, especially if module caches or package variants require a clean rebuild.

## Scope discipline

- Preserve user-written comments verbatim unless the user requests otherwise.
- Prefer an explicit dependency or small build-target boundary over global serialization, broad compatibility headers, or suppressed linker diagnostics.
- Do not change the default platform/toolchain just to make one failure disappear; first reproduce with the currently configured project toolchain.
