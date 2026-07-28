# Windows Clang-cl named-module dependency-scan reference

Load this reference for Windows `clang-cl` builds that fail while Xmake runs
`clang-scan-deps` / P1689 dependency scanning for a C++ module unit.

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

This is specific to the current Windows Xmake + `clang-cl` dependency-scan path.
The MSVC build path has not required this visibility workaround.  It is a
build-configuration limitation, not evidence that the package is part of a
module's public C++ API.

## Long-term boundary

Prefer a package-provided named module when it is available and correctly
integrated with Xmake's module graph.  For example, toml++ provides
`import tomlplusplus;`, but the current Xmake package does not expose that
module build.  A proper package module integration can replace the textual
header include and remove this header-search-path propagation workaround.

Alternatively, move third-party-header-dependent definitions out of an exported
module interface unit and into an implementation unit compiled in the owning
target.  Do not make that architectural split solely to silence one scanner
failure without checking lifecycle and module-boundary design first.

## Verification

```powershell
rtk err xmake clean --all -y
rtk err xmake f -m release -y
rtk err xmake -y
```

After the repair, confirm that the original `clang-scan-deps` missing-header
error is gone.  If the full build reaches a different first error, record that
as a separate failure rather than treating it as a regression of this repair.