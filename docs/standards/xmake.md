# xmake Specifications

How SoulEngine modules, their tests, and the final binary are declared in xmake. The wrappers live in `xmake/soul_module.lua`, loaded by the root `xmake.lua` via `includes()` before any module file runs; module `xmake.lua` files use them instead of raw `target()` calls.

## Build helpers: location and naming

All project-owned Lua build helpers live under the top-level `xmake/` directory, one file per domain (`soul_module.lua` today; future `rules/`, platform scripts, etc. follow the same roof). They are loaded with `includes()` from the root `xmake.lua`, before `includes("Engine")`.

This is dictated by xmake's two scope kinds. Module files call `soul_module(...)` in the *description scope* (the declarative top level of an xmake.lua), where the interpreter exposes only a whitelist of APIs — `import()` is **not** available there, so `add_moduledirs` + `import()` cannot deliver helpers to module files. `includes()` is the only description-scope code-sharing channel (custom `interp_add_scopeapis` scopes exist but collect data, they do not create targets). `import()` is available inside *script scope* (`on_xxx`/`before_xxx`/`after_xxx` function bodies); if a helper ever needs xmake's internal modules, move that logic into a script-scope callback.

Because `includes()` shares one global namespace with xmake's own APIs, every description-scope helper this project defines carries the `soul_` prefix (`soul_module`, `soul_finalize_modules`, `soul_finalize_tests`, `soul_impl_targets`, `soul_add_package_rpath`). State such as the module registry stays file-local inside `xmake/soul_module.lua` and is only reachable through those functions.

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

Registration is deferred: the `tests` sub-table is queued during the module's script and targets materialize after all `includes()` (via `soul_finalize_tests()`), because a module's tests may link modules declared later (RHI tests link RHIVulkan.Impl).

## Final binary

The `SoulEngine` target takes its implementation-target deps from the module registry. Adding a module never requires editing `Engine/Source/Runtime/xmake.lua` — only the module's own `xmake.lua` and the `includes()` list.

## Gotchas

- The module scanner's local cache lives in `.xmake/<plat>/<arch>/cache/` (not `build/`). After changing target dependency structure, delete that directory **and** re-run `xmake f -c`; a stale cache otherwise crashes the module scanner with `attempt to index a nil value (local 'cxx_sourcebatch')`, and deleting it without reconfiguring loses package include paths (`fatal error: 'entt/entt.hpp' file not found`).
- When appending an options table to a wrapper's list parameter, insert it into the list before `table.unpack`: a multi-value call in any argument position but the last truncates to its first value, silently dropping every entry but the first.
