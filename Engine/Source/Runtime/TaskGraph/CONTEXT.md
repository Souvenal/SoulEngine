# Context: TaskGraph

**Namespace:** `SoulEngine` (exposes `TaskGraph` class, `ThreadQueue` enum)

Module `TaskGraph`. Thread-safe multi-queue task dispatcher for cross-thread work.

## Terms

| Term | Definition |
|------|------------|
| **TaskGraph** | Multi-queue task dispatcher with 3 named queues (`ThreadQueue::Game`, `Render`, `RHI`). Each queue mutex+deque. Consumer threads drain via `TryDequeue(queue)` per-frame, capped at `kMaxTasksPerPoll`. |
| **ThreadQueue** | Enum identifying target thread: `Game`, `Render`, `RHI`. Used as key for `Enqueue()` and `TryDequeue()`. |

## Dependencies

- `Core` — logging, type aliases
