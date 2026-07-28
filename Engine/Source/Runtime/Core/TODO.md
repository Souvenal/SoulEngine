# TODO

## Keep toml++ private when its C++ module package is available in Xmake

`Core:Config` does not expose any toml++ type in its exported API.  It currently
uses `#include <toml++/toml.h>` in `Config/Config.cppm`'s global module fragment,
however.  In the current Xmake + clang-cl configuration, Clang's P1689 dependency
scanner (`clang-scan-deps`) runs for that source in consuming-target contexts, so the
scanner must also receive toml++'s header search path.  MSVC builds have not required
this workaround; it is a Clang dependency-scan limitation, not a public API requirement.

For now, `Core/xmake.lua` marks `toml++` and its `TOML_COMPILER_HAS_EXCEPTIONS=0`
configuration as `public` solely to propagate the matching scan/compile environment.
They are not semantic or public C++ API dependencies of `Core:Config`.

When the Xmake toml++ package provides and wires up toml++'s C++20 module
interface (`import tomlplusplus;`), migrate `Config.cppm` from the textual
include to that named-module import.  Then reconfigure the package as a module
build dependency without exporting a header-search-path workaround, and verify
with a clean release build.