# Context: Launch

**Namespace:** SoulEngine (exposes EngineLoop)

Engine startup and the three-thread Game → Render → RHI pipeline.

## Terms

| Term | Definition |
|------|------------|
| **EngineLoop** | Lifecycle: PreInit → Init → Run → Shutdown. Run starts Render/RHI workers and runs GameLoop on the main thread. |
| **GameLoop** | Main-thread loop: poll events, update application/editor state, wait for a slot in Empty or RenderReady, build the SceneSnapshot, then publish GameReady. |
| **RenderLoop** | Worker loop: wait for GameReady, drain Render tasks, consume SceneData, publish RenderReady, wait for the prior packet's RHI/GPU completion, then replace RenderResult and wake RHILoop. |
| **RHILoop** | Worker loop: wait for a slot packet marked not consumed, drain RHI tasks, call RHIRenderDevice::Tick(), drain deferred deletions, borrow the packet for Execute(), submit it through EndFrame(), store RHIFrameCompletion, mark the packet consumed, and notify RenderLoop. It calls WaitIdle() before exiting. |
| **FrameSlot** | One of three synchronized handoff records containing SceneSnapshot, Renderer, ImGui snapshot, RenderResult, and its RHIFrameCompletion. The slot remains the RenderResult owner until RenderThread waits for GPU completion. |
| **Render packet** | RenderResult produced by RenderLoop and borrowed by RHILoop for Execute(). The packet is not moved into backend storage; its RHIRefs remain in the FrameSlot until the next replacement. |
| **SlotState** | Empty -> GameReady -> RenderReady -> GameReady; Empty is only the initial bootstrap state, while RenderReady lets GameThread prepare the next SceneData. RHI packet handoff uses a separate consumed flag. |
| **FatalError** | Atomic fatal flag set by any loop; wakes waiting slots and causes coordinated shutdown. |

## RHI reference lifetime across slots

A normal command list carries RHIRef<T> values rather than raw RHI observers.
RHILoop borrows Slot.RenderPacket.CmdList for Execute() and leaves the packet
in the FrameSlot after EndFrame() returns an RHIFrameCompletion. RenderLoop
waits for the RHI-consumed flag and that completion token immediately before
replacing the packet, so slot ownership itself proves the RHIRef lifetime.

The ImGui overlay remains a borrowed Execute-time payload: its snapshot and
texture queue are valid through Execute() and are cleared with the slot only
after that call returns.

## Shutdown order

1. Request worker stop, start ResourceManager shutdown, and wake slots.
2. Join RenderLoop and RHILoop while TaskGraph is still running, so a worker
   caught mid-frame finishes its in-flight slot against live task services
   instead of failing with spurious "TaskGraph is not running" errors; RHILoop
   waits for GPU idle on exit. TaskGraph stops only after both workers join.
3. Close the application; clear slot snapshots/render packets; release Editor and renderer GPU owners.
4. Clear ResourceManager so ordinary RHIRef owners release while the render device/deletion queue still exists.
5. Destroy the RHI device, then the window system and Editor.

No RHI ref that may own a native payload may outlive step 5.

## Dependencies

- Core — config, logging, thread roles
- WindowSystem — window creation and events
- Application — application lifecycle and mutable Scene
- RHI — singleton lifecycle and RHI-thread execute/tick
- Resource — request shutdown, dependency polling, resource cleanup
- Scene — per-frame snapshot
- Renderer — renderer selection and RenderLoop frame recording
- TaskGraph — Game/Render/RHI task queues
- Editor — main-thread UI and Render-thread overlay packet

## Known gaps

| Gap | Status |
|-----|--------|
| FrameSlot is not the submission-retention owner; RHILoop clears the moved-from packet before GameLoop reacquires the slot. | Documented divergence from the requested slot-reset policy; Vulkan backend retention is the current safeguard. |
| RHI initialization and final device shutdown happen on the main thread, not the RHI worker. | Lifecycle exception; strict RHI-thread-only context ownership is not yet enforced. |
| CLI argument parsing | Open |
| Application abstraction | Open |
