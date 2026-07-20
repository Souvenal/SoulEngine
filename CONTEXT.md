# SoulEngine — Context

Project-level glossary and cross-module ADR index. Per-module contexts indexed in [`CONTEXT-MAP.md`](CONTEXT-MAP.md).

## Global Constraints

These apply to every module. Per-module `CONTEXT.md` files may add module-specific terms and rules.

| Constraint | Detail |
|------------|--------|
| **Language** | C++23 modules throughout (`.cppm` files, `export module`, `import std;`) |
| **Exceptions** | No exceptions, no RTTI |
| **Error handling** | `std::expected<T, Core::ErrorMessage>` for all fallible functions; `Core::ErrorMessage` carries a human-readable error chain for logging, not programmatic branching |
| **Runtime constant-buffer ABI** | Slang/Vulkan runtime constant buffers use the default `std140` layout. Shader structs declare semantic fields only; CPU mirrors use `alignas(16)` where required and lock reflected offsets with `sizeof` and `offsetof` static assertions. Do not add named padding fields. |

## Architecture Decision Records

Cross-module ADRs live in [`docs/adr/`](docs/adr/). Module-scoped decisions should be recorded in `docs/adr/` within the relevant module directory.
