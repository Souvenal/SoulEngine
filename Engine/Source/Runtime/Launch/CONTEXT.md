# Context: Launch

**Namespace:** SoulEngine (exposes EngineLoop)

Engine startup and the three-thread Game → Render → RHI pipeline.

## Terms

| Term | Definition |
|------|------------|
| **EngineLoop** | Lifecycle: PreInit → Init → Run → Shutdown. Run starts Render/RHI workers and runs GameLoop on the main thread. |
| **GameLoop** | Main-thread loop: poll events, update application/editor state, wait for a reusable slot, build the SceneSnapshot, then publish GameReady. It accepts Empty or RHIDone slots. |
| **RenderLoop** | Worker loop: wait for GameReady, drain Render tasks, render the slot snapshot into RenderResult/RHICommandList, then publish RenderReady. |
| **RHILoop** | Worker loop: wait for RenderReady, drain RHI tasks, call RHIRenderDevice::Tick(), poll Resource dependency waiters, execute the moved command list, then publish RHIDone. It calls WaitIdle() before exiting. |
| **FrameSlot** | One of three synchronized handoff records containing SceneSnapshot, Renderer, ImGui snapshot, and RenderResult. The slot coordinates CPU pipeline ownership; it is not the Vulkan submission-retirement record. |
| **Render packet** | RenderResult whose move-only RHICommandList is produced by RenderLoop. RHILoop moves that list into Execute(). For Vulkan, Execute() moves it again into backend in-flight submission storage. |
| **SlotState** | Empty -> GameReady -> RenderReady -> RHIDone; GameLoop may immediately overwrite a RHIDone slot and does not write an intermediate Empty state. |
| **FatalError** | Atomic fatal flag set by any loop; wakes waiting slots and causes coordinated shutdown. |

## RHI reference lifetime across slots

A normal command list now carries RHIRef<T> values rather than raw RHI
observers. RHILoop performs Execute(std::move(Slot.RenderPacket.CmdList)) and
then assigns Slot.RenderPacket = {} before it sets RHIDone. Thus the slot
command-list storage is cleared by RHILoop, **not** by the next GameLoop slot
acquisition.

The current Vulkan backend preserves correctness by retaining the moved command
list in its own graphics-timeline VulkanInFlightSubmission. Reusing a
FrameSlot therefore does not imply GPU completion, and FrameSlot reuse must
not be used as the destruction proof. The intended clear-only-on-GameLoop-
reacquire ownership policy is not implemented literally; backend submission
retention is the active safety mechanism.

The ImGui overlay remains a borrowed Execute-time payload: its snapshot and
texture queue are valid through Execute() and are cleared with the slot only
after that call returns.

## Shutdown order

1. Request worker stop, start ResourceManager shutdown, stop TaskGraph, and wake slots.
2. Join RenderLoop and RHILoop; RHILoop waits for GPU idle on exit.
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