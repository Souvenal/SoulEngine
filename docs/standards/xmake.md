# xmake Specifications

How SoulEngine modules, their tests, and the final binary are declared in xmake. The wrappers live in `Engine/Source/Runtime/xmake.lua`; module `xmake.lua` files use them instead of raw `target()` calls.

## Module declaration: soul_module

One `soul_module(name, {...})` call declares a C++ module as up to two targets:

- **Interface target** `name`: `moduleonly`, compiles the `*.cppm` interface units, carries the BMI for consumers.
- **Implementation target** `name.Impl`: `static`, compiles the `*.cpp` implementation units. Created only when the `impl` sub-table is present; interface-only modules (Shader, EditorTypes) omit it.

```lua
soul_module("RHIVulkan", {
    deps     = {"RHI", "WindowSystem", "TaskGraph"},   -- interface: module names
    packages = {"volk", "tracy", ...},                 -- interface: packages
    defines  = {"TRACY_VK_USE_SYMBOL_TABLE"},          -- interface: private defines
    impl = {
        deps     = {"Core", "RHI", "VMA"},           -- implementation: module names + plain targets
        packages = {"volk", "tracy", ...},           -- implementation: private packages
        defines  = {"TRACY_VK_USE_SYMBOL_TABLE"},    -- implementation: private defines
        excludes = {"VMA.cpp"},                      -- glob exclusions
    },
    tests = { ... },                                   -- optional, see Tests below
})
```

### Dependency notation

Deps are module names, never target names. The wrapper resolves them through the module registry: on the interface target a dep stays `X`; on the implementation target a dep maps to `X.Impl` when that module has one. Names with no registry entry (plain helper targets like VMA/STBImage) pass through verbatim. The interface target itself is prepended to the implementation deps automatically.

### Visibility rule

Interface targets take every dep and package as `public`; implementation targets keep everything private. There are no per-entry visibility knobs.

The interface target exists to be consumed: anything its module units touch can leak into the BMI through a global module fragment (header include dirs, macros) or into the link through its object files, so consumers need it all reachable. The implementation target is a terminal static library — only final binaries and tests link it — so nothing must propagate out of it.

### File conventions and escape hatches

- Interface sources default to `**.cppm`, public. Implementation sources default to `**.cpp|Tests/**`.
- `excludes` drops paths from the default glob (VMA.cpp, STBImage.cpp, EditorTypes.cppm).
- `files` replaces the interface glob when the directory hosts more than one module (EditorTypes uses `files = "EditorTypes.cppm"`).
- `impl.files = false` suppresses the implementation glob entirely; follow the `soul_module` call with a `target("X.Impl")` re-entry block for conditional logic (Platform selects `Mac/**.cpp` / `Windows/**.cpp` per OS this way).
- Sibling helper targets that are not C++ modules (VMA, STBImage) stay plain `target()` calls outside the wrapper.

## Tests

Tests are a `tests` sub-table on the same `soul_module` call; a module with a `Tests/` directory passes one, others omit it. Each `Tests/**.cpp` file becomes one binary target `TestsFor<Module>_<File>` (never aggregate many files onto one target: module BMI/object ownership is target-scoped, and mixed artifacts go stale). Test targets automatically depend on the module's interface and implementation targets; `deps`/`additional_deps` list only *other* modules, still as module names with the same `.Impl` mapping.

Supported keys: `deps`/`additional_deps`, `packages`/`additional_packages`, `exclude_tests` (basename patterns), `rpath_packages`.

Registration is deferred: `test_module` queues during the module's script and targets materialize after all `includes()`, because a module's tests may link modules declared later (RHI tests link RHIVulkan.Impl).

## Final binary

The `SoulEngine` target takes its implementation-target deps from the module registry. Adding a module never requires editing `Engine/Source/Runtime/xmake.lua` — only the module's own `xmake.lua` and the `includes()` list.

## Gotchas

- The module scanner's local cache lives in `.xmake/<plat>/<arch>/cache/` (not `build/`). After changing target dependency structure, delete that directory **and** re-run `xmake f -c`; a stale cache otherwise crashes the module scanner with `attempt to index a nil value (local 'cxx_sourcebatch')`, and deleting it without reconfiguring loses package include paths (`fatal error: 'entt/entt.hpp' file not found`).
- When appending an options table to a wrapper's list parameter, insert it into the list before `table.unpack`: a multi-value call in any argument position but the last truncates to its first value, silently dropping every entry but the first.
