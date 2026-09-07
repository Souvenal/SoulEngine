# SoulEngine — Context

Project-level glossary and cross-module ADR index. Per-module contexts are indexed in [`CONTEXT-MAP.md`](CONTEXT-MAP.md).

## Global Constraints

These apply to every module. Per-module `CONTEXT.md` files may add module-specific terms and rules.

| Constraint | Detail |
|------------|--------|
| **Language** | C++23 modules throughout (`.cppm` files, `export module`, `import std;`) |
| **Exceptions** | No exceptions, no RTTI |
| **Error handling** | `std::expected<T, Core::ErrorMessage>` for all fallible functions; `Core::ErrorMessage` carries a human-readable error chain for logging, not programmatic branching |
| **Runtime constant-buffer ABI** | Slang/Vulkan runtime constant buffers use the default `std140` layout. Shader structs declare semantic fields only; CPU mirrors use `alignas(16)` where required and lock reflected offsets with `sizeof` and `offsetof` static assertions. Do not add named padding fields. |
| **RHI lifetime** | Persistent GPU resources cross module boundaries as `RHIRef<T>`, never as owning raw pointers. A resource-bearing RHI command retains the needed refs until its backend-defined submission-completion point. The final native destruction is queued and drained on the RHI thread while the render device is alive. |
| **RHI shutdown boundary** | Every `RHIRef` that may own a native payload must be released before `RHIRenderDevice::Destroy()`. The current global deferred-deletion queue has no post-device fallback. |

## Architecture Decision Records

Cross-module ADRs live in [`Docs/ADR/`](Docs/ADR/). Module-scoped decisions should be recorded in `Docs/ADR/` within the relevant module directory.

- [ADR 03 — Three-thread pipeline and variant commands](Docs/ADR/03-three-thread-pipeline-variant-commands.md)
- [ADR 11 — Editor active-Scene system inspection](Docs/ADR/11-editor-active-scene-system-inspection.md)
