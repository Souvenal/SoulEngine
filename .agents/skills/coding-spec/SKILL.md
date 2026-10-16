---
name: coding-spec
description: MUST load for any SoulEngine project code work — reading, writing, modifying, reviewing, refactoring, debugging, or generating code in this repository. Routes to the domain vocabulary, the ADRs, and the coding standards before touching code. Skip only for non-code documentation, issue/project management, or purely conceptual discussion where no repository code is read or changed.
---

# Coding Spec

This skill is only a router.

Before reading, writing, modifying, reviewing, refactoring, debugging, or generating project code:

1. Inspect the target file paths.
2. Read root `GLOSSARY-MAP.md` and the `GLOSSARY.md` of every module whose directory contains one of the target files. Use the glossary's vocabulary; never invent terms.
3. Read the ADRs in `docs/adr/` that touch the area. If your work contradicts an ADR, surface it explicitly rather than silently overriding.
4. Before writing or modifying ANY comment, read `docs/standards/comments.md`. This is the one implementation-time standard — comment rework is expensive, and `Tools/check_comments.py` enforces its mechanical rules.
5. Language standards — `docs/standards/cpp.md`, `docs/standards/slang.md`, `docs/standards/vulkan.md` — are indexed by `CODING_STANDARDS.md` and consumed at review time by /code-review. Read them up front when working in their area if you want the code to pass review the first time.
