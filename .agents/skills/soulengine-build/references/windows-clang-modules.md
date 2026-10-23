# Windows Clang-cl named-module dependency-scan reference

Load this reference for Windows `clang-cl` builds that fail while Xmake runs `clang-scan-deps` / P1689 dependency scanning for a C++ module unit.

## Header-only package visibility during module scanning

| Signature | Root cause | Smallest repair | Do not do |
| --- | --- | --- | --- |
| `<Consumer> generating.module.deps <Producer>.cppm`, followed by `fatal error: '<package header>' file not found` from `clang-scan-deps` | The producer module unit textually includes a third-party header, but Xmake runs Clang's dependency scan in a consuming target's compilation context. A private `add_packages()` entry contributes its include path only while compiling the producer target itself; it is absent from the consuming scanner command. The package need not appear in the module's exported C++ API. | Mark the package `public` on the target that owns the module unit: `add_packages("<package>", {public = true})`. Also export any package-configuration defines that change the header API: `add_defines("<define>", {public = true})`. Reconfigure, clean generated artifacts, and rebuild. | Conclude that the consuming module semantically depends on the package, add includes to consumers, or suppress dependency scanning. |

Current project instances:

| Module target | Textual package headers in its `.cppm` sources | Public package workaround |
| --- | --- | --- |
| `Core` | `toml++/toml.h` in `Config/Config.cppm` | `toml++` and `TOML_COMPILER_HAS_EXCEPTIONS=0` |
| `Resource` | `stb_image.h` and `hlsl++.h` | `stb` and `hlslpp` |
| `ShaderCompiler` | `magic_enum/magic_enum.hpp` | `magic_enum` |

This is specific to the current Windows Xmake + `clang-cl` dependency-scan path. The MSVC build path has not required this visibility workaround.  It is a build-configuration limitation, not evidence that the package is part of a module's public C++ API.

## Long-term boundary

Prefer a package-provided named module when it is available and correctly integrated with Xmake's module graph.  For example, toml++ provides `import tomlplusplus;`, but the current Xmake package does not expose that module build.  A proper package module integration can replace the textual header include and remove this header-search-path propagation workaround.

Alternatively, move third-party-header-dependent definitions out of an exported module interface unit and into an implementation unit compiled in the owning target.  Do not make that architectural split solely to silence one scanner failure without checking lifecycle and module-boundary design first.

## Verification

```powershell
rtk err xmake clean --all -y
rtk err xmake f -m release -y
rtk err xmake -y
```

After the repair, confirm that the original `clang-scan-deps` missing-header error is gone.  If the full build reaches a different first error, record that as a separate failure rather than treating it as a regression of this repair.

## Cross-module template instantiation needs exported helpers

| Signature | Root cause | Smallest repair | Do not do |
| --- | --- | --- | --- |
| `error: no matching function for call to '<helper>'` raised while a renderer module TU instantiates an exported template (the `note: in instantiation of function template specialization` points at the consumer TU), where the helper is defined in the producer partition's anonymous namespace. | Templates with internal linkage (anonymous namespace) are invisible at the point of instantiation in other module units; MSVC tolerated this, clang does not. | Export the whole helper template chain plus every non-dependent type it references (RenderGraph aggregate-walk machinery in `Graph.cppm` is the current instance). | Do not try to keep helpers internal via module linkage — visibility at a foreign PoI is exactly what failed; do not wrap helper blocks in `namespace {}` inside `export namespace` (ill-formed: `anonymous namespaces cannot be exported`). |

## Windows clang toolchain link and test quirks

| Signature | Root cause | Smallest repair | Do not do |
| --- | --- | --- | --- |
| `lld-link: error: undefined symbol: vk<...>` (small set, all referenced from Tracy Vulkan headers) at final exe link. | Tracy headers call loader entry points directly; the engine loads Vulkan dynamically (vulkan-hpp `DynamicLoader`) and deliberately links no `vulkan-1.lib`. | Define `TRACY_VK_USE_SYMBOL_TABLE` on the RHIVulkan target and let `VKProfiling.cppm` populate `tracy::VkSymbolTable` from the instance dispatcher (`GetInstance().getDispatcher()` gives both `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr`); `TracyVkContextCalibrated` then takes `(instance, physdev, device, queue, cmdbuf, instanceProcAddr, deviceProcAddr)`. Zero new dependencies, zero linkage. | Do not link `vulkan-1.lib`/add a `vulkan-loader` package for this (tried 2026-09-16, reverted — {public=true} propagation from moduleonly targets is unreliable across config switches anyway); do not reach for volk before checking that Tracy 0.14.1's built-in symbol-table mode covers the needed symbols. |
| `clang++: error: no such file or directory: '/WHOLEARCHIVE:gmock_main.lib'` when linking tests under the clang/llvm toolchains. | The GNU-style clang++ driver treats a bare `/WHOLEARCHIVE:...` ldflag as an input file; only link.exe/clang-cl parse it directly. | In `test_module` the flag is branched: `-Wl,/WHOLEARCHIVE:...` for `is_config("toolchain", "clang", "llvm", "gcc")`, plain form otherwise. | Do not delete the MSVC branch — link.exe rejects `-Wl,`. |
| clang rejects `hlslpp::` / other GMF-header names inside `.cpp` tests that only `import <Module>;`. | Textual GMF headers are reachable but not exported; MSVC allowed qualified lookup of reachable names, clang follows the standard. | Add the textual include (`#include <hlsl++.h>`) to the test TU; package include dirs already propagate `{public = true}`. | Do not try to re-export third-party headers from the module interface. |
