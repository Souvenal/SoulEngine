# Context: TaskGraph

**Namespace:** `SoulEngine` (exposes `TaskGraph` class, `ThreadQueue` enum)

Module `TaskGraph`. Thread-safe multi-queue task dispatcher for cross-thread work. `TaskGraph::Init()`
spawns the requested number of background workers and logs the spawned count internally.

## Terms

| Term | Definition |
|------|------------|
| **TaskGraph** | Engine task system responsible for both background worker execution and thread-affinity dispatch to named engine threads. |
| **ThreadQueue** | Enum identifying target engine thread: `Game`, `Render`, `RHI`. Used when work must run on a specific thread rather than on a background worker. |
| **Background worker** | TaskGraph-owned worker thread for non-thread-affine CPU work such as file IO, image decode, and shader compilation. |
| **Thread-affinity task** | Task that must run on a specific engine thread because it touches thread-owned state such as scene mutation, renderer state, or RHI objects. |

## Dependencies

- `Core` — logging, type aliases
