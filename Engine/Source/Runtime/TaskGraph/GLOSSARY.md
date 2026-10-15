# TaskGraph

Process-global, thread-safe multi-queue dispatcher for cross-thread work: background workers for free work, named thread queues for thread-affine work.

## Language

**TaskGraph**:
The engine task system, responsible for both background worker execution and thread-affinity dispatch to named engine threads.

**ThreadQueue**:
Enum identifying a target engine thread (Game, Render, RHI), used when work must run on a specific thread rather than a background worker.

**Background worker**:
TaskGraph-owned worker thread for non-thread-affine CPU work such as file IO, image decode, and shader compilation.

**Thread-affinity task**:
Task that must run on a specific engine thread because it touches thread-owned state (scene mutation, renderer state, RHI objects). See also frame-affined task in the RHI glossary.
