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

**Updated August 1, 2026.** FrameSlot remains the CPU handoff record. It is not
the ownership record that proves submitted GPU work has retired.

- Every normal resource-bearing `RHICommandList` field retains `RHIRef<T>`:
  attachments, pipelines, vertex/index buffers, shader parameter resources,
  present source, TLAS, and TLAS-instance BLAS values.
- RHILoop calls `Execute(std::move(Slot.RenderPacket.CmdList))`, then clears the
  moved-from slot packet before publishing `RHIDone`. The requested policy to
  reset the command list only when GameLoop reacquires the slot is not
  implemented literally.
- Vulkan signals a graphics timeline value and moves the complete submitted
  list into `VulkanInFlightSubmission`. It releases that list only after the
  corresponding timeline value completes. This backend retention, not slot
  reuse, keeps command refs alive while the GPU can use them.
- Final `RHIRef` release may happen on any thread, but it transfers the native
  payload into `RHIDeferredDeletionQueue`. Normal-runtime `RHIRenderDevice::Tick()`
  drains that queue on RHILoop after backend completion polling.
- `RHIImGuiPresentationOverlayCmd` is an Execute-time borrowed-pointer bridge
  for Dear ImGui data. It is not the lifetime pattern for ordinary engine RHI
  resources.

The Vulkan implementation is safe for normal ref-backed commands, but this
submission-retention rule is not yet an abstract-RHI contract shared by future
backends. Device creation and final backend shutdown remain main-thread
lifecycle exceptions; only normal runtime RHI work is RHI-thread confined.
## Thread lifecycles

### GameLoop (main thread)

Responsible for: window event polling, game logic tick, scene state capture.

```
PollEvents → FrameMark → compute dt → wait for Empty/RHIDone → OnTick(dt) → copy Scene → GameReady → notify_all
```

Details:
- `PollEvents` must run on main thread (GLFW requirement)
- `OnTick(dt)` updates application-level scene state (transform, animation, physics)
- After tick, copies the scene camera (and future: mesh/light/transform lists) into the slot's `Scene::Scene` for the Render thread
- Blocks when all slots are occupied (RHI thread hasn't freed one yet)

### RenderLoop (`std::jthread`)

Responsible for: draining render-side cross-thread tasks, converting scene data into GPU commands (as variant command lists).

```
wait for GameReady → drain Render task queue → Render(scene) → store CommandList in slot → RenderReady → notify_all
```

Details:
- Drains the Render queue from `TaskGraph` (for tasks enqueued by Game or RHI threads)
- Calls `IRenderer::Render(scene)` which produces a `RHI::CommandList` — a data structure containing `std::vector<Pass>`, each Pass holding variant `Command` types
- The CommandList is pure data (no GPU handles beyond SPtr references) — no Vulkan calls happen here
- On render failure: broadcasts `FatalError` and exits

### RHILoop (`std::jthread`)

Responsible for: draining RHI-side cross-thread tasks, uploading command data to GPU (descriptor writes, constant buffer upload), submitting GPU work and presenting.

```
wait for RenderReady → drain RHI task queue → for each Pass: allocate secondary CB → begin rendering → dispatch commands → end rendering → submit primary CB → present → RHIDone → notify_all
→ on stop: WaitIdle() → return
```

Details:
- Drains the RHI queue from `TaskGraph`
- Takes the `CommandList` from the slot and calls `RenderDevice::Execute()`:
  1. **Frame begin:** Wait on timeline semaphore (CPU-GPU sync), acquire swapchain image, begin primary command buffer
  2. **Per-pass recording:** For each `Pass` in the command list — allocate a secondary command buffer from the frame's SubPool, call `beginRendering`, dispatch variant commands via `CommandVisitor::operator()`, bind draw-scope descriptors from the active pipeline's reflected layout, call `endRendering`, end secondary
  3. **Submit:** ExecuteCommands from primary, signal timeline semaphore, present
  4. **Frame end:** Advance `m_CurrentFrame`

- All Vulkan API calls are confined to this thread — no other thread touches vkCmd*, vkQueueSubmit, or vkQueuePresent
- Per-pass secondary command buffers are stored in `FrameContext::ScratchSecondaries` — freed when the same FrameContext is reused next frame (after timeline wait guarantees GPU completion)
- On thread stop: calls `WaitIdle()` to drain remaining GPU work before join returns

## Shutdown sequence

1. GameLoop exits (window close or FatalError)
2. `m_FatalError = true`, `notify_all`, `TaskGraph.Shutdown()`
3. request_stop + join RenderLoop
4. request_stop + join RHILoop (RHILoop calls WaitIdle before returning)
5. Release Application, FrameSlot, Editor, Renderer, and ResourceManager owners while the render-device deletion queue is valid
6. RenderDevice::Destroy after GPU idle; backend shutdown drains in-flight submissions and deferred native destruction
7. WindowDisplay::Shutdown

**Error rule:** Any loop reaching a fatal error sets `m_FatalError` + `notify_all`. All loops check `m_FatalError` in their CV predicates. On fatal error, the slot state machine does NOT advance — the remaining loops break immediately without transitioning their current slot.

## Related files

- `Engine/Source/Runtime/Launch/Launch.cppm` — EngineLoop, GameLoop/RenderLoop/RHILoop, FrameSlot, SlotState
- `Engine/Source/Runtime/TaskGraph/TaskGraph.cppm` — cross-thread task dispatch (3 named queues)
