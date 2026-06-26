---
name: coding-spec
description: |-
  MUST load for any SoulEngine project code work: reading, writing, modifying, reviewing, refactoring, debugging, fixing diagnostics, or generating code in this repository. This skill routes Codex to the authoritative coding rules in .agents/rules before touching code.

  Applies to project source and shader work, including C++, Slang, Vulkan/RHI code, tests, examples, and code review. Skip only for non-code documentation, issue/project management, or purely conceptual discussion where no repository code is read or changed.
---

# Coding Spec

This skill is only a router. The authoritative rules live in `.agents/rules/`.

Before reading, writing, modifying, reviewing, refactoring, debugging, or generating project code:

1. Inspect the target file paths.
2. Read root `CONTEXT.md`.
3. Read `CONTEXT-MAP.md`.
4. Read every per-module `CONTEXT.md` whose module directory contains one of the target files.
5. Read every rule file in `.agents/rules/` whose frontmatter `paths` glob matches those files.
6. Apply all matched context and rule guidance while working.

Current rule files:

- `.agents/rules/cpp-coding-spec.md` for `Engine/Source/**/*.h`, `Engine/Source/**/*.hpp`, `Engine/Source/**/*.cpp`, and `Engine/Source/**/*.cppm`.
- `.agents/rules/slang-coding-spec.md` for `Engine/Shaders/**/*.slang` and `Applications/*/Shaders/**/*.slang`.
- `.agents/rules/vulkan-coding-spec.md` for `Engine/Source/Runtime/RHI/Vulkan/**/*.cpp` and `Engine/Source/Runtime/RHI/Vulkan/**/*.cppm`.

If multiple rules match a file, load and apply all of them. For example, Vulkan C++ source must follow both the C++ rules and the Vulkan rules.

If the relevant files are not known yet, inspect the repository first, then load the root context, context map, matching module contexts, and matching rule files before making code decisions.
