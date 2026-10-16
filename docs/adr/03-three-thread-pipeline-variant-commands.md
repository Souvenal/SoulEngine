# ADR 03 — Three-Thread Frame Pipeline

## Status

Accepted (2026-06-23)

## Context

SoulEngine's original main loop was single-threaded: `Tick()` called `OnTick()` then `OnRender()` sequentially on the main thread. As the engine grew, this blocked game logic during GPU work.

We needed a pipelined architecture where three threads (Game, Render, RHI) operate concurrently, each advancing one frame apart.

## FrameBuffer Diagram

```
    ┌────────────┐   ┌────────────┐   ┌────────────┐
    │FrameSlot[0]│   │FrameSlot[1]│   │FrameSlot[2]│
    ├────────────┤   ├────────────┤   ├────────────┤
Game→│  SceneData │   │            │   │            │
    │ GameReady  │   │   Empty    │   │  RHIDone   │
    └─────┬──────┘   └────────────┘   └────────────┘
          │
          ▼
    ┌────────────┐   ┌────────────┐   ┌────────────┐
    │FrameSlot[0]│   │FrameSlot[1]│   │FrameSlot[2]│
    ├────────────┤   ├────────────┤   ├────────────┤
Render→│  CmdList   │   │            │   │            │
    │RenderReady│   │  GameReady │   │  RHIDone   │
    └─────┬──────┘   └────────────┘   └────────────┘
          │
          ▼
    ┌────────────┐   ┌────────────┐   ┌────────────┐
    │FrameSlot[0]│   │FrameSlot[1]│   │FrameSlot[2]│
    ├────────────┤   ├────────────┤   ├────────────┤
RHI→ │  RHIDone   │   │RenderReady│   │  GameReady │
    └────────────┘   └────────────┘   └────────────┘
```

**Slot state machine:**

```
Empty ─(Game writes SceneData)→ GameReady ─(Render writes CommandList)→ RenderReady ─(RHI submits and moves CmdList)→ RHIDone ─(Game reuses slot)→ GameReady
```

**Pacing:** GameThread may lead RHIThread by at most 2 frames (3 slots total).

## Key Questions

### 1. Why 3 slots?

2 slots would mean Game and RHI share one slot — same as 2 threads. 3 is the minimum for 3-thread pipelining: each thread needs its own exclusive slot to write to.

### 2. Why mutex + condition_variable?

RHI thread waits on GPU work (timeline semaphore), milliseconds. Spinning = wasted CPU. CV with predicate is correct.

### 3. Why 3 dedicated threads, not a job system?

Pipeline is strictly ordered (Game→Render→RHI). Dedicated threads make ordering explicit and simple. A job system can be added alongside later for compute.

### 4. Why `notify_all`?

All 3 threads wait on the same condition variable. `notify_one` can wake the wrong thread → lost-wakeup deadlock. `notify_all` safe — spurious wakeups cheap (2 threads check predicate false, re-wait).

### 5. Why GameThread = main thread?

GLFW requires `PollEvents()` on window creation thread. Spawning a separate Game `jthread` would leave main thread idle — wastes a core.

## Ownership

| Concept | Owned by |
|---------|----------|
| FrameSlot array (3 slots) | `EngineLoop` (Launch module) |
| SlotState enum | `EngineLoop` |
| Thread spawning & join | `EngineLoop::Run()` / `Shutdown()` |
| FatalError broadcast | `EngineLoop` |
| Cross-thread task queues | `EngineLoop` via `TaskGraph` |

## Submission-owned command resource lifetime

**Updated August 1, 2026; revised 2026-10-15 (two-hop deferred destruction).** FrameSlot remains the CPU handoff record. It is not the ownership record that proves submitted GPU work has retired.

- Every normal resource-bearing `RHICommandList` field retains `RHIRef<T>`: attachments, pipelines, vertex/index buffers, shader parameter resources, present source, TLAS, and TLAS-instance BLAS values.
- RHILoop calls `Execute(std::move(Slot.RenderPacket.CmdList))`, then clears the moved-from slot packet before publishing `RHIDone`. The requested policy to reset the command list only when GameLoop reacquires the slot is not implemented literally.
- Vulkan signals a graphics timeline value and moves the complete submitted list into `VulkanInFlightSubmission`. It releases that list only after the corresponding timeline value completes. This backend retention, not slot reuse, keeps command refs alive while the GPU can use them.
- Final `RHIRef` release may happen on any thread. Destruction is then two hops with distinct jobs. The first hop is the module-level `RHIDeferredDeletionQueue`: the cross-thread transport, drained on RHILoop after the backend Tick() each frame, with a final flush at backend Shutdown(). The second hop is the backend's GPU-retirement queue: the drained destructor does not destroy in place — it hands the native handles to the backend, which stamps destruction as a virtual timeline event above every submitted value, so the next real submission covers it. This is sound because no new command can reference a resource after its last ref is gone, so the destruction frame's own submission upper-bounds the last GPU use. The second hop replaces frame-slot retention for resources referenced outside the command list (upload-batch destinations, device-address-pulled geometry buffers): they need no manual retention.
- Shutdown boundary: the module-level queue has no post-device fallback, so every `RHIRef` that may own a native payload must be released before `RHIRenderDevice::Destroy()`.
- `RHIImGuiPresentationOverlayCmd` is an Execute-time borrowed-pointer bridge for Dear ImGui data. It is not the lifetime pattern for ordinary engine RHI resources.

The Vulkan implementation is safe for normal ref-backed commands, but this submission-retention rule is not yet an abstract-RHI contract shared by future backends. Device creation and final backend shutdown remain main-thread lifecycle exceptions; only normal runtime RHI work is RHI-thread confined.

## Thread lifecycles

### GameLoop (main thread)

Responsible for window event polling, game logic tick, and scene state capture. GLFW requires event polling on the window-creation thread, which is why GameLoop is the main thread. After the tick it publishes the captured scene state into the slot and marks it GameReady; it blocks when no slot is free.

### RenderLoop (`std::jthread`)

Responsible for draining render-side cross-thread tasks and converting scene data into GPU commands. Its output command list is pure data — no Vulkan calls happen on this thread. On render failure it broadcasts FatalError and exits.

### RHILoop (`std::jthread`)

Responsible for draining RHI-side cross-thread tasks, recording and submitting the borrowed command list, and presenting. All Vulkan API calls are confined to this thread — no other thread touches command recording, submission, or present. On stop it waits for GPU idle before returning.

## Shutdown sequence

1. GameLoop exits (window close or FatalError); `m_FatalError = true`, `notify_all`
2. request_stop + join RenderLoop
3. request_stop + join RHILoop (RHILoop calls WaitIdle before returning)
4. `TaskGraph.Shutdown()` — only after both workers join, so a worker caught mid-frame finishes its in-flight slot against live task services instead of failing spuriously (revised 2026-10-15; previously ordered before the joins)
5. Release Application, FrameSlot, Editor, Renderer, and PipelineRegistry owners while the render-device deletion queue is valid
6. RenderDevice::Destroy after GPU idle; backend shutdown drains in-flight submissions and deferred native destruction
7. Window system shutdown

**Error rule:** Any loop reaching a fatal error sets `m_FatalError` + `notify_all`. All loops check `m_FatalError` in their CV predicates. On fatal error, the slot state machine does NOT advance — the remaining loops break immediately without transitioning their current slot.
