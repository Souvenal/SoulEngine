# TODO

## Migrate toml++ to its C++ module package when available in Xmake

`Core:CVar` does not expose any toml++ type in its exported API.  It currently
uses `#include <toml++/toml.h>` in `CVar/CVar.cppm`'s global module fragment.

Historically, `Core/xmake.lua` marked `toml++` and its
`TOML_COMPILER_HAS_EXCEPTIONS=0` configuration as `public` because Clang's
P1689 dependency scanner (`clang-scan-deps`) compiled the then-Config.cppm in
consuming-target contexts and needed the header search path.  Verified
2026-09-22: this workaround is no longer required — toml++ and the define are
private and the full build is green.  If consumer-target module scans ever
error on `toml++/toml.h` again, re-add both as `public` first.

When the Xmake toml++ package provides and wires up toml++'s C++20 module
interface (`import tomlplusplus;`), migrate `CVar.cppm` from the textual
include to that named-module import and verify with a clean release build.
