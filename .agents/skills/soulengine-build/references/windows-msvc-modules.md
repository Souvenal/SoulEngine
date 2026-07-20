# Windows MSVC modules diagnostic reference

Load this reference for Windows x64 builds using MSVC, Xmake, and C++ named modules. Use the exact failure signature to choose the smallest repair. Rebuild from clean generated artifacts after interface, runtime, or toolchain changes.

| Signature | Likely cause | Repair | Do not do |
| --- | --- | --- | --- |
| `C2230: cannot find module "std"`; `C1199: missing IFC reference` | MSVC/Xmake scan recorded only a re-exporting dependency such as `Core`, not its transitive named `std` dependency. | Add direct `import std;` to the failing unit. Inspect its generated `*.module.json`: `requires` must contain `std`. | Force all builds to `-j1`, enable a fallback scanner, or assume a dependency's `export import std` is enough for Xmake's MSVC scan. |
| LLVM Clang 22.1.7 compiler internal error while compiling SoulEngine modules | The tested Windows Clang path remains blocked by a compiler failure after the earlier module/STL issues are addressed. | Keep Windows on the configured MSVC path. Reconsider Clang only after the internal error is fixed upstream or otherwise resolved; then validate `test_std/` and a clean full-engine build. | Treat the earlier concepts/type-traits workaround as a complete Clang fix. |
| `std::same_as`, concepts, or traits missing under Windows Clang | Windows Clang + MSVC STL named-module integration did not expose the facility reliably. | Keep Windows on the configured MSVC path. Use `test_std/` before considering Clang support again. | Add `<concepts>` / `<type_traits>` to project module global fragments while the MSVC path is active. |
| C1116 inside `<stop_token>` while importing `std` | A module unit textually included spdlog (or another header that brings in standard-library headers) and also imported named `std`; MSVC saw conflicting standard-library representations. | Isolate the integration in a conventional `.cpp` backend and bridge it with private C ABI declarations. `CoreLoggingBackend` is the pattern. | Include spdlog in `Core:Logging`, remove `import std` from the module, or hide the error with compiler flags. |
| `LNK2038` for `_ITERATOR_DEBUG_LEVEL` or `RuntimeLibrary` | A static target/library used `MD`, while the Debug application/named `std` module used `MDd` (or the reverse). | Set Debug `MDd`, non-Debug `MD`; clean and rebuild the backend plus dependent targets. | Link with `/NODEFAULTLIB` or mix CRTs deliberately. |
| C2461 around a member declaration such as `Uint32 Binding = 0;` inside `struct Binding` | The member name collides with the enclosing type name; MSVC parses it as constructor-like syntax. | Rename the numeric field to `BindingIndex` and update producer, consumer, and tests. | Rename a widely used public type when the field rename is the smaller semantic change. |
| clang-cl: `no such file or directory: '/fo'` while compiling an `.rc` file | clang/clang-cl and the Windows resource compiler were selected incompatibly by a third-party package build. | Use the configured MSVC toolchain on Windows, then clean/reconfigure the package build. | Patch package source or resource scripts before verifying the actual toolchain selection. |
| `LNK1561: must define an entry point` for a test binary although `gmock_main.lib` appears on the command line | The generated link order placed gmock main before test object files, so its archive member was not selected. | Keep `gtest[main]`; on Windows add `/WHOLEARCHIVE:gmock_main.lib` through `add_ldflags` in `test_module`. | Add a project-local `main`, add a `main` to every test source, or depend on archive ordering. |
| `C2235` says a test target's `std.ifc` has incompatible target architecture | Fresh concurrent test-target module builds produced an invalid/stale IFC state. | Clean generated artifacts and run the test suite with `xmake test -j1 -v`. | Change platform/arch settings without evidence, or treat this as a source-level type error. |

## Inspecting scan output

For a failing module, find its generated scan file under:

```text
build/.gens/<target>/windows/x64/debug/rules/bmi/cache/scans/
```

Open the `*.module.json` matching the source. Its `requires` list should include every directly imported named module. For an `import std;` repair, require a `logical-name` of `std`.

## Known project boundaries

- `Core:Logging` is an exported engine module and must not include spdlog headers.
- `CoreLoggingBackend` is a conventional static target that owns spdlog headers, sinks, formatting, and file I/O.
- `Windows.cppm` and `SlangTypes.cppm` intentionally import `std` directly for MSVC scan correctness.
- The old `<concepts>` / `<type_traits>` global-module-fragment additions were Windows-Clang workarounds and were removed after moving Windows back to MSVC.
- `test_module` uses `gtest[main]` and forces `gmock_main.lib` on MSVC; project test sources must not define their own `main`.
