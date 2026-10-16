# Launch

Engine bootstrap and the three-thread Game → Render → RHI frame pipeline (ADR 03).

## Language

**EngineLoop**:
Engine lifecycle owner: PreInit → Init → Run → Shutdown. Run starts the Render/RHI workers and runs the game loop on the main thread.

**GameLoop**:
Main-thread loop — polls events, updates application and editor state, builds the SceneSnapshot, and publishes GameReady.

**RenderLoop**:
Worker loop — consumes the snapshot, records the frame's command list, publishes RenderReady, and replaces the slot's render packet only after the prior packet's GPU completion.

**RHILoop**:
Worker loop — drains RHI tasks, ticks the render device, drains deferred deletions, records and submits the borrowed packet, and hands back the frame-completion token.

**FrameSlot**:
One of three synchronized handoff records carrying the scene snapshot, render packet, and completion token between the three loops (ADR 03).

**Render packet**:
The per-frame render result produced by RenderLoop and borrowed by RHILoop for recording; it is never moved into backend storage.

**SlotState**:
Slot handoff state (Empty → GameReady → RenderReady); the RHI packet handoff uses a separate consumed flag.

**FatalError**:
Atomic fatal flag set by any loop; wakes waiting loops and drives coordinated shutdown.
