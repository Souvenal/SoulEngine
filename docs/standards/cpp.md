# C++ Coding Specifications

## Behavior-Changing Rules

Program semantics or compiler diagnostics rules.

- `[[nodiscard]]` for funcs with return values
- No exceptions; use `std::expected<T, ErrorMessage>` (defined in `Core:Util.Error`) for all functions that can encounter an error, including void-returning ones. The error chain is for human reading (logs), not for programmatic branching — callers should not parse it. Convention:
  - **Error (program cannot continue normally):** return an `ErrorMessage` from the callee; the terminal caller calls `.ToString()` on it and enters crash-handle flow.
  - **Warning (non-fatal, program continues):** `LogWarning` directly in the callee, no `std::expected`.
  - **Leaf error (first failure):** `return std::unexpected(ErrorMessage("Something failed"))`.
  - **Appending context:** when forwarding an error across layers, call `.Append()` on it. The `Append` method adds the outer context to the end of the chain; `Format()` renders outermost first with progressive "Caused by:" indentation.
  - `Append` and `ErrorMessage` constructors now take a plain `String` — for dynamic messages, wrap with `Format()` first:
    ```cpp
    // Leaf error (static):
    return std::unexpected(ErrorMessage("TOML parse error at line 42"));
    // Leaf error (dynamic):
    return std::unexpected(ErrorMessage(Format("Failed to load '{}'", FilePath)));
    // Middle layer — append:
    return std::unexpected(R.error().Append("Failed to load config file"));
    // Middle layer — append with formatting:
    return std::unexpected(R.error().Append(Format("Failed to load '{}'", FilePath)));
    // Terminal:
    LogError("{}", R.error().ToString());
    ```
- In human-readable errors and logs, stringify enum values with `magic_enum::enum_name()` instead of casting them to numbers.
- RTTI disabled (`-fno-rtti`). Use `std::variant`, `std::visit`, or virtual dispatch for type-erased polymorphism.
- No raw `new` or `delete` expressions. Use `UPtr<T>` (`std::make_unique`) for sole ownership and `SPtr<T>` (`std::make_shared`) for shared ownership. Raw pointers are permitted only as non-owning observers (borrowed references). Interop with C APIs that require raw allocation (e.g., `malloc`/`free` callbacks, Vulkan allocation callbacks) is allowed but must be encapsulated in a RAII wrapper with a `// NOLINT-RAW-MEM` comment explaining why it cannot use standard smart pointers.
- Use project type aliases defined in `Engine/Source/Runtime/Core/Util/Types.cppm` instead of their underlying standard types. Currently enforced: `UPtr<T>` over `std::unique_ptr<T>`, `SPtr<T>` over `std::shared_ptr<T>`. Others (`Int32`, `String`, `StringView`, `ErrorMessage`, etc.) are also available and preferred. `ErrorMessage` lives in `Core:Util.Error` (re-exported via `Core:Util` and `Core`). Core types are available under `using namespace SoulEngine::Core;`.

If code violates a rule, emit a diagnostic with the rule ID and a suggested code rewrite. Example: `RULE-TRAILING-RET: Replace "int f()" with "auto f() -> int"`. Auto-fix where possible.

If input code throws exceptions, transform them to explicit error-return idioms and annotate with comments. If automatic transformation is unsafe, report `RULE-NO-EXCEPTIONS` and provide a manual migration patch.

## Coding Style

Code form and readability only, not behavior.

- Initialize structs using aggregate initialization (`MyStruct{ .Field = value }`) instead of default-construct-then-assign. If the struct has no fields or all fields use their defaults, `= {}` is preferred over `MyStruct Var;` followed by field assignments.

- Apply trailing return types to:
  - free functions
  - non-constructor/non-destructor member functions, including static members
  - operator overloads
  - lambdas

  Do not apply trailing return types to constructors, destructors, or conversion operators. Example: `MyClass() = default;` remains unchanged.


- In `class` types, prefix all non-static data members with `m_`. `struct` types are exempt — their data members need no prefix. Static data members should use no prefix regardless of type. This does not apply to union members, bitfields of size 0, or anonymous struct/union members.

- `bool` data members use an `Is` prefix (e.g. `m_IsCoherent`); `bool` fields of aggregate structs follow the same convention.

- **DRY (Don't Repeat Yourself) — define each function once and avoid redundant declarations.** A function has one definition. Keep its declaration at the definition site unless a language boundary requires a separate declaration:
  - For ordinary non-template module members moved out of an interface to reduce importer recompilation, declare the member in the interface and define it once in the attached implementation unit. This is the sole exception to the one-signature rule.
  - Do not add separate forward declarations across unrelated translation units.
  - Within a single translation unit, forward declarations are allowed only when necessary for mutually recursive free functions; require a comment explaining the recursion and mark with `ALLOW-FWD-RECURSION`. Prefer defining functions at first use.
  - For public APIs intended for reuse across translation units, use module interface units; export the declaration in the interface and define it there or in its attached implementation unit.

- Module partition naming: allow exactly one colon separator in the form `A:B`. To express logical nesting beyond one level, use a single dot inside the partition name, for example `A:B.C`. Treat the dot as part of the partition identifier and ensure build tools map `A:B.C` to a single module partition name.

- Comment placement and content rules (Doxygen `///` usage, per-member vs type-level comments) live in [`comments.md`](comments.md).

- All data member default values SHOULD be written at the declaration site (`Type m_Member = Default;`) so the default is visible at the definition. Use constructor initializer lists for simple, infallible value assignment and dependency injection, including base-class construction. Use a factory returning `std::expected` when construction requires validation, allocation, resource creation, or any other fallible work. For configurable values that may be overridden by a config file, use `m_Member = Cfg.SomeField.value_or(m_Member);` in Init/constructor body; the member's declaration-site default serves as the backup.

- Every `enum class` must have its first enumerator as `Unknown = 0` (or the equivalent default sentinel). Rationale: zero-initialisation is the default in C++; an explicit `Unknown` sentinel makes it impossible to accidentally treat a default-constructed value as a valid state. Non-negative sentinel names (e.g. `None`) are acceptable when the enum represents a bitmask and zero means "nothing set".

## C++ Modules

- Ordinary non-template module members may be declared in a module interface and defined once in an attached implementation unit. This is the supported interface/implementation split for reducing importer recompilation.
- A module interface partition uses `export module M:Part;`. Re-export it from the primary interface only when downstream modules need its API.
- A feature implementation partition uses a distinct name such as `module M:Part.impl;`. The interface and implementation partition names must never be identical: do not declare `M:Part` in both units.
- A primary module implementation unit may use `module M;` for definitions declared by `export module M;`. This rule is distinct from feature partition implementations, which use the `.impl` suffix.
- Implementation partitions are private to the named module. They are added to the same build target but are not imported or re-exported by the primary interface. An implementation partition imports its interface partition itself with `import :Part;` when it needs those declarations.
- A feature with ordinary runtime logic normally uses an interface/implementation pair: `Feature.cppm` declares `export module M:Feature;`, while `Feature.cpp` declares `module M:Feature.impl;`. Pure type, template, concept, and compile-time partitions may remain interface-only when no implementation unit provides value.
- A primary interface may aggregate public feature interfaces. The aggregate interface is a facade and contains export imports, not feature implementation bodies.
- Exporting a class does not export every declaration reachable through its definition. `exported` and `reachable` are separate properties in [module.interface] and [module.reach]. A private member may use a third-party type when importers do not need to name or instantiate that type; do not introduce PImpl solely to hide such members. Move ordinary function bodies and private helper definitions to the implementation partition instead.
- Shared cross-backend APIs are declared and exported once in the primary module interface. Backend implementation partitions provide the one corresponding definition for the active platform.
- Use ordinary `namespace SoulEngine` blocks and explicit per-declaration `export`; `export namespace` is prohibited.
- In xmake targets that contain module implementation units, use `set_kind("static")` with `set_policy("build.c++.modules", true)`. Register interface `.cppm` files with `{public = true}` and implementation `.cpp` files as ordinary target sources. Glob patterns are allowed; module declarations, not matching filenames, establish the interface/implementation roles.

## Profiling (Tracy)

- Tracy identifies instrumented strings by pointer address, not content (see the Tracy manual, "Unique pointers"): identical string literals are not guaranteed to pool to a single address, and a mismatched pointer silently splits frame sources. Never pass a fresh string literal to a Tracy macro at more than one place.
- Define each name once as an `inline constexpr const char*` constant and pass the variable to `FrameMarkStart`/`FrameMarkEnd`/`FrameMarkNamed`/`ZoneScopedN`/`tracy::SetThreadName`. Do not use plain namespace-scope `constexpr` for this — `const` implies internal linkage, giving one address per translation unit.
- Thread names use `<Domain>Thread` (e.g. `GameThread`, `RenderThread`, `RHIThread`); continuous frame spans use `<Domain>Frame` (e.g. `GameFrame`, `RenderFrame`, `RHIFrame`).
- Zones record work events only. Do not add a top-level zone that duplicates a frame span; the per-frame interval is owned by `FrameMarkStart`/`FrameMarkEnd`.
- An unnamed master `FrameMark` is emitted on the RHI thread right after `EndFrame` (submit + present); it feeds the GUI's default numbered frame set. The named continuous spans `GameFrame`/`RenderFrame`/`RHIFrame` coexist with it and carry the per-stage intervals. GPU contexts do not depend on the master mark: `TracyVulkan`/`TracyD3D12` attach GPU zones via timestamp calibration (`VK_EXT_calibrated_timestamps` + `TracyVkContextCalibrated`), not frame marks.
- `tracy::SetThreadName` copies the string it is given, so indexed thread names (e.g. `Format("WorkerThread{}", i)`) may be formatted locals. Pointer identity applies only to names embedded in events — `FrameMarkStart`/`FrameMarkEnd`/`FrameMarkNamed`/`ZoneText` and zone names in `SourceLocationData` — those require stable pointers.

## Quick Reference

| Rule | Scope | Enforced |
|------|-------|----------|
| `[[nodiscard]]` on return values | All functions | Static analysis |
| No exceptions, use `std::expected<T, ErrorMessage>` | All code | Code generation / review |
| `-fno-rtti` (no `typeid`/`dynamic_cast`) | All code | Compiler flag |
| No raw `new`/`delete` | All code | Review |
| Trailing return types | Functions (not ctor/dtor) | Review |
| DRY — one definition; module interface split is the sole declaration exception | All functions | Review |
| `m_` prefix on non-static data members | `class` types only | Review |
| Aggregate init (`{ .Field = ... }`) | Structs | Review |
| Declaration-site defaults | All data members | Review |
| `UPtr<T>` / `SPtr<T>` aliases | All code | Review |
| `Unknown = 0` first enumerator | All `enum class` | Review |
| Tracy strings via named constants (pointer identity) | All Tracy macros | Review |
| Zone = work event only; no zone duplicating a frame span | All Tracy zones | Review |
