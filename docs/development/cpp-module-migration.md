# C++ Module Migration Guide

This guide defines the repository workflow for splitting C++ module interfaces from implementation units while preserving SoulEngine's module boundaries and xmake's dependency graph.

## Choose The Unit Shape

Use a primary module interface and a primary module implementation unit when the module has one public interface and ordinary non-template implementations:

```cpp
// TaskGraph.cppm
export module TaskGraph;

namespace SoulEngine {

export class TaskGraph {
  public:
    auto Init() -> void;
};

} // namespace SoulEngine

// TaskGraph.cpp
module TaskGraph;

namespace SoulEngine {

auto TaskGraph::Init() -> void {
}

} // namespace SoulEngine
```

Use an interface partition only when the partition has a real independent interface boundary. An interface partition is declared with `export module M:Part;` and is re-exported by the primary interface only when downstream modules need that partition API. A primary or aggregate interface such as `Util.cppm` may serve as a facade that only re-exports feature interfaces.

Use an implementation partition for platform-specific or otherwise private implementation code. Give it a distinct partition name such as `module M:Part.impl;`. The `.impl` name is mandatory for a feature implementation partition: an interface partition and its implementation partition must never declare the same partition name.

An implementation partition is not imported or re-exported by the primary interface. Add its source file to the same xmake target. If the implementation partition needs declarations from an interface partition, it imports that partition itself with `import :Part;`. A primary implementation unit may instead use `module M;` for declarations from `export module M;`; do not use that form for feature partitions.

## Keep The Public Boundary Singular

Declare a cross-backend API once in the primary module interface and export that declaration from the ordinary `namespace SoulEngine` block. Each platform implementation partition defines that declaration exactly once. Do not create one exported declaration per backend for a function that is part of the shared module API.

Do not use `export namespace`. Every exported declaration receives its own `export` specifier inside an ordinary `namespace SoulEngine` block.

Keep platform-only helpers, state, and third-party includes in the implementation partition. Do not expose them through an interface partition unless they appear in a public signature.

## Configure xmake

A target containing `.cpp` module implementation units must be a normal static target with module dependency scanning enabled:

```lua
target("Platform")
    set_kind("static")
    set_policy("build.c++.modules", true)

    add_deps("Core")
    add_files("Platform.cppm", {public = true})
    add_files("Mac/**.cpp")
```

Mark `.cppm` interface sources as public module sources. Add `.cpp` implementation units as ordinary target sources. Glob patterns are valid for both source classes. Module declarations determine interface versus implementation roles; filenames do not. Do not use `moduleonly` for a target that contains implementation units.

## Validate The Migration

Run the focused target build first, then the affected downstream target, and finally the complete build:

```text
xmake repo -u
xmake clean --all
xmake -y Platform
xmake -y TaskGraph
xmake -y
```

A focused build must reach compilation and archiving without duplicate-module diagnostics. A downstream build must resolve every public interface partition. The complete build must link the final executable.

## Common Diagnostics

`duplicate module name detected for "M:Part"` usually means an interface unit and an implementation unit both declare the same partition name. Rename the implementation unit to a distinct implementation partition such as `M:Part.impl`.

`missing M:Part dependency for module M` means a downstream target cannot see an interface partition required by the primary module exported dependency graph. Check that the interface `.cppm` source is registered with `{public = true}`. Do not make an implementation partition public to solve this diagnostic.

A target configured as `moduleonly` cannot represent a module implementation unit correctly. Convert it to a static target and enable `build.c++.modules` before investigating source-level diagnostics.

An undefined symbol for a moved API at the final link usually means the interface declaration is non-inline while the definition in the implementation unit is still marked `inline` (an inline definition emits no external symbol) or is missing entirely. Never write `export` in an implementation unit either — `export declaration can only be used within a module interface`. Exportedness comes from the interface declaration alone; the implementation provides a plain non-inline definition.

An `expected unqualified-id` / `unknown type name` cascade right after adding `export` usually means the specifier landed in the wrong position: it precedes the whole declaration, so templates are `export template <...>`, never `template <...> export class` or `[[nodiscard]] export auto`.

## Extracting Implementations

Move member bodies whole and verify mechanically: qualify the name (`Class::Method`), drop `explicit` from out-of-class constructor definitions, and diff every moved body against the git baseline (`git show HEAD:<path>`) before building. A half-moved signature (declaration replaced, body left behind) produces a `non-member function cannot have a const qualifier` cascade.

New implementation units are untracked files, and pre-commit (including `--all-files`) only checks tracked files — clang-format never sees them. Stage the new files (`git add -N` suffices) or run clang-format on them directly before trusting hook output.

Filenames need no `.impl` suffix; only the partition declaration carries it. A feature pair is `Part.cppm` + `Part.cpp`.

A broad `add_files("**.cpp")` glob pulls `Tests/**` into the production target (gtest dependency-scan errors). Exclude tests explicitly: `add_files("**.cpp|Tests/**")`.

Folding a partition into a facade module (for example `Core:Paths` becoming `Core:Util.Paths`): replace a blanket `export import` of the facade in the folded unit with precise sibling imports (`:Util.Types`, `:Util.Singleton`, ...) so the unit does not import the facade that re-exports it; update the facade and the primary interface's re-export lists; grep for comments that name the old partition.

## Cross-TU Type Identity

EnTT's `type_hash` derives from `__PRETTY_FUNCTION__`, and the spelling of a standard type can differ between module TUs that see it through different include/import paths (`std::string` versus `std::basic_string<char>`); link order decides which spelling wins. A dispatch that compares `type_hash` values computed in different modules can therefore mismatch silently at runtime with every build and focused test green (seen during the Core migration: a light-component field was rejected as an unsupported persisted field type). When the compared types cross module boundaries, compare `type_info::index()` — the process-unique runtime type index — instead of the hash.

The same class of issue applies to static initialization: converting a target from `moduleonly` to a static library changes which object files the linker pulls. Registration objects whose only effect is a dynamic initializer must live in translation units that also define referenced code, or the linker may drop the object from the archive and skip the registration.

## Breaking Import Cycles

Only interfaces can create import cycles. A member used solely by the implementation can become a module-linkage helper in the implementation unit, calling the public API, so the interface drops the import that caused the cycle. Implementation partitions may then freely cross-import each other's interfaces — they compile after both interfaces. (The Core migration used this to decouple CVar from Logging, letting the log-level CVars move from Launch into Logging's implementation unit.)

## Migration Checklist

- Identify ordinary non-template implementations that can leave the interface unit.
- Keep public declarations and complete public documentation in the interface unit.
- Use a feature `.cppm`/`.cpp` pair by default for ordinary runtime logic; use interface-only units for pure types, templates, concepts, and compile-time code.
- Use one primary implementation unit only for ordinary definitions declared by the primary module interface; use distinct `.impl` partitions for feature implementations.
- Keep every platform-independent public API declaration in the primary module interface.
- Use ordinary `namespace SoulEngine` blocks and explicit per-declaration `export`.
- Register interface `.cppm` files as public and implementation `.cpp` files as ordinary sources.
- Clean generated module artifacts after changing unit shape or partition names.
- Run focused, downstream, and full builds and record the exact results; the full build must link the final executable, since missing or wrongly-inline definitions surface only there.
- Diff every moved function body against `git show HEAD:<path>`; the goal is relocation, not rewrite.
- Stage new implementation files before running pre-commit, and run it in the worktree that contains the change.
- Check that exported declarations are limited to the public API. Distinguish `exported` from `reachable`: private member types may remain reachable without becoming importer-visible names, so do not add PImpl solely to hide them.
