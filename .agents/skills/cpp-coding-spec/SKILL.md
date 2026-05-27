---
name: cpp-coding-spec
description: |-
  Enforce C++ coding standards for the SoulEngine project (trailing return types, S-prefix classes, m_ member prefix, no exceptions, no RTTI, no raw new/delete, Doxygen comments, aggregate init, declaration-site defaults).

  MUST load when reading, writing, modifying, reviewing, refactoring, or generating any C++ source file (.cpp, .h, .hpp, .cppm, .cc, .cxx, .ixx), or when the user asks to fix C++ compiler warnings/errors, run C++ static analysis, or submit C++ code for review.

  Skip when the conversation is about architecture discussion, design planning, project management, non-code documentation, CI/CD pipelines, build system (xmake), dependency management, or any topic that does not involve writing or editing C++ source code directly.
---

# C++ Coding Specifications

## Behavior-Changing Rules

Program semantics or compiler diagnostics rules.

- `[[nodiscard]]` for funcs with return values
- No exceptions; use `std::expected<T, SErrorMessage>` (defined in `Core:Util.Error`) for all functions that can encounter an error, including void-returning ones. The error chain is for human reading (logs), not for programmatic branching — callers should not parse it. Convention:
  - **Error (program cannot continue normally):** return an `SErrorMessage` from the callee; the terminal caller calls `.ToString()` on it and enters crash-handle flow.
  - **Warning (non-fatal, program continues):** `LogWarning` directly in the callee, no `std::expected`.
  - **Leaf error (first failure):** `return std::unexpected(SErrorMessage("Something failed"))`.
  - **Appending context:** when forwarding an error across layers, call `.Append()` on it. The `Append` method adds the outer context to the end of the chain; `Format()` renders outermost first with progressive "Caused by:" indentation.
  - `Append` and `SErrorMessage` constructors now take a plain `String` — for dynamic messages, wrap with `Format()` first:
    ```cpp
    // Leaf error (static):
    return std::unexpected(SErrorMessage("TOML parse error at line 42"));
    // Leaf error (dynamic):
    return std::unexpected(SErrorMessage(Format("Failed to load '{}'", FilePath)));
    // Middle layer — append:
    return std::unexpected(R.error().Append("Failed to load config file"));
    // Middle layer — append with formatting:
    return std::unexpected(R.error().Append(Format("Failed to load '{}'", FilePath)));
    // Terminal:
    LogError("{}", R.error().ToString());
    ```
- No RTTI; disallow `dynamic_cast` and `typeid` expressions. Prefer `std::variant` or `std::any` for sum types; custom type-erasure wrappers are allowed only when necessary and should be documented.
- No raw `new` or `delete` expressions. Use `UPtr<T>` (`std::make_unique`) for sole ownership and `SPtr<T>` (`std::make_shared`) for shared ownership. Raw pointers are permitted only as non-owning observers (borrowed references). Interop with C APIs that require raw allocation (e.g., `malloc`/`free` callbacks, Vulkan allocation callbacks) is allowed but must be encapsulated in a RAII wrapper with a `// NOLINT-RAW-MEM` comment explaining why it cannot use standard smart pointers.
- Use project type aliases defined in `Engine/Source/Runtime/Core/Util/Types.cppm` instead of their underlying standard types. Currently enforced: `UPtr<T>` over `std::unique_ptr<T>`, `SPtr<T>` over `std::shared_ptr<T>`. Others (`Int32`, `String`, `StringView`, `SErrorMessage`, etc.) are also available and preferred. `SErrorMessage` lives in `Core:Util.Error` (re-exported via `Core:Util` and `Core`).

If code violates a rule, emit a diagnostic with the rule ID and a suggested code rewrite. Example: `RULE-TRAILING-RET: Replace "int f()" with "auto f() -> int"`. Auto-fix where possible.

If input code throws exceptions, transform them to explicit error-return idioms and annotate with comments. If automatic transformation is unsafe, report `RULE-NO-EXCEPTIONS` and provide a manual migration patch.

## Coding Style

Code form and readability only, not behavior.

- Initialize structs using aggregate initialization (`SMyStruct{ .Field = value }`) instead of default-construct-then-assign. If the struct has no fields or all fields use their defaults, `= {}` is preferred over `SMyStruct Var;` followed by field assignments.

- Apply trailing return types to:
  - free functions
  - non-constructor/non-destructor member functions, including static members
  - operator overloads
  - lambdas

  Do not apply trailing return types to constructors, destructors, or conversion operators. Example: `SMyClass() = default;` remains unchanged.

- Prefix class names with `S` when the class is part of the public API or is used across module boundaries. Exempt types that are standard-layout aggregates with only public data members and no user-provided constructors/destructors. Do not apply the prefix rule to enum types or type aliases.

- In `class` types, prefix all non-static data members with `m_`. `struct` types are exempt — their data members need no prefix. Static data members should use no prefix regardless of type. This does not apply to union members, bitfields of size 0, or anonymous struct/union members.

- Do not use separate forward declarations for functions across translation units. Within a single translation unit, allow forward declarations only when necessary for mutually recursive free functions; require a comment explaining the recursion and mark with `ALLOW-FWD-RECURSION`. Prefer defining functions at first use. For public APIs intended for reuse across translation units, use module interface units.

- Module partition naming: allow exactly one colon separator in the form `A:B`. To express logical nesting beyond one level, use a single dot inside the partition name, for example `A:B.C`. Treat the dot as part of the partition identifier and ensure build tools map `A:B.C` to a single module partition name.

- Use Doxygen-style comments (`///`) for public API documentation.

- All data member default values MUST be written at the declaration site (`Type m_Member = Default;`), never in constructor initializer lists or constructor body assignments. Rationale: jump-to-definition shows the default immediately, and there is exactly one place to look. Constructor initializer lists are banned — a member has no default at that point, which forces the reader to cross-reference two locations. For configurable values that may be overridden by a config file, use `m_Member = Cfg.SomeField.value_or(m_Member);` in Init/constructor body; the member's declaration-site default serves as the backup.

## File Scope

These rules apply to all files matching the globs:

```
Engine/Source/**/*.h
Engine/Source/**/*.hpp
Engine/Source/**/*.cpp
Engine/Source/**/*.cppm
```

Files outside these paths (e.g., build scripts, config files, tests in a separate root) are not subject to these rules unless explicitly noted.

## Quick Reference

| Rule | Scope | Enforced |
|------|-------|----------|
| `[[nodiscard]]` on return values | All functions | Static analysis |
| No exceptions, use `std::expected<T, SErrorMessage>` | All code | Code generation / review |
| No RTTI (`dynamic_cast`, `typeid`) | All code | Compiler flags / review |
| No raw `new`/`delete` | All code | Review |
| Trailing return types | Functions (not ctor/dtor) | Review |
| `S` prefix on class names | Public/cross-module API | Review |
| `m_` prefix on non-static data members | `class` types only | Review |
| Doxygen `///` comments | Public API | Review |
| Aggregate init (`{ .Field = ... }`) | Structs | Review |
| Declaration-site defaults | All data members | Review |
| `UPtr<T>` / `SPtr<T>` aliases | All code | Review |
