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

Use an interface partition only when the partition has a real independent interface boundary. An interface partition is declared with `export module M:Part;` and is re-exported by the primary interface only when downstream modules need that partition API.

Use an implementation partition for platform-specific or otherwise private implementation code. Give it a distinct partition name such as `module M:Part.impl;`. Do not declare the same partition name in both an interface unit and an implementation unit.

An implementation partition is not imported or re-exported by the primary interface. Add its source file to the same xmake target. If the implementation partition needs declarations from an interface partition, it imports that partition itself with `import :Part;`.

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

Mark `.cppm` interface sources as public module sources. Add `.cpp` implementation units as ordinary target sources. Do not use `moduleonly` for a target that contains implementation units.

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

## Migration Checklist

- Identify ordinary non-template implementations that can leave the interface unit.
- Keep public declarations and complete public documentation in the interface unit.
- Use one primary module implementation unit or distinct `.impl` partitions for private implementations.
- Keep every platform-independent public API declaration in the primary module interface.
- Use ordinary `namespace SoulEngine` blocks and explicit per-declaration `export`.
- Register interface `.cppm` files as public and implementation `.cpp` files as ordinary sources.
- Clean generated module artifacts after changing unit shape or partition names.
- Run focused, downstream, and full builds and record the exact results.
