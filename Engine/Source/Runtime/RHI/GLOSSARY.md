# RHI

Render Hardware Interface — the abstract GPU layer. Backends register through a self-registering factory. The RHI boundary owns native-resource construction, submission lifetime, and native destruction; higher layers carry RHIRef handles and record declarative commands.

## Language

### Resource model

**RHIRef**:
Copyable handle to a shared payload that owns a backend-native resource, without exposing native ownership to callers. An availability state (committing / ready / failed) guards access: the truthiness operator admits access only when ready, and an explicit nullable access path remains for backend lowering.

**Ready resource**:
An RHIRef whose payload exists and is readable. Readiness is separate from submission lifetime: an in-flight submitted ready payload stays alive through command-list retention, not through the ref itself.

**Resource kinds**:
Exactly three — buffer, texture, and acceleration structure. Vertex / index / constant / storage / indirect / sampled / render-target are usages, not types: the creation descriptor carries the usage-flag union and the backend dispatches on it.
_Avoid_: type-per-usage resource classes.

**View**:
Value-semantic (resource, usage, extent, access) tuple — not an object; commands and bindings hold views and lifetime comes from the embedded resource ref. One view, one role; stage is a pass property and image layout is derived, never declared (ADR 05).
_Avoid_: view objects, layout-carrying views.

**Buffer lifetime**:
Persistent buffers are individually owned and outlive frames; transient buffers are per-frame belt slices valid only for the frame in which they were created. Lifetime is chosen on the creation descriptor; a transient ref retained past its creation frame is a contract violation.

**Buffer memory usage**:
The allocation-placement property (GPU-only vs host-visible), named after the VMA / The-Forge / D3D12 heap-type vocabulary. Placement decides capability and upload mechanism: GPU-only derives transfer-dst and device-address capability and uploads through the frame-start staging batch; host-visible uploads by direct memory copy and carries neither capability.

**Buffer device address**:
Every GPU-only buffer exposes a device address at creation — it costs nothing, so it is not a usage opt-in. Host-visible and transient buffers carry none.

**Texture**:
The single image identity: attachments, sampled uploads, storage images, and copy endpoints are usages on the descriptor, never types. Mip level count is a creation-time property (partial chains legal); subresource selection is a view property.

**Swapchain image**:
Backend-private presentation image, acquired and released by the backend. It is never an engine-owned texture and never surfaces as an RHIRef.
_Avoid_: backbuffer texture.

**Sampler**:
State object, not a resource — no views, no upload, no per-frame lifetime. Exists so shader bindings retain their payload through the embedded ref like every other binding kind.

**Readback**:
GPU-to-CPU readback consumer object — the same category as sampler (no views, no bindings, no upload). A transfer pass records a texture readback into a backend-internal ring that is never a graph resource; reads are game-thread polled with newest-completed semantics, and the one-write-per-frame-per-stream contract keeps the reader and allocator disjoint.

**Pipeline**:
Backend-polymorphic payload retained by bind, draw, dispatch, push-constant, parameter-binding, or trace commands. Host-provided specialization-constant values are fixed at creation and are part of the pipeline identity — two value sets must yield two pipelines.

### Frame model

**RenderDevice**:
Process-wide RHI singleton. Resource creation runs synchronously on the calling thread; descriptors never carry data — uploads and acceleration-structure builds are decoupled into frame-affined tasks (ADR 05).

**CommandList**:
Move-only frame packet of ordered pass entries (graphics, compute, transfer, ray tracing) with an optional ImGui presentation overlay. Every resource-bearing command field is ref-backed, so command recording never depends on raw observers.

**Pass**:
A graphics, compute, transfer, or ray-tracing scope containing declarative command work. Attachments, pipelines, draw buffers, shader-parameter resources, and the presentation source are ref-backed; color and depth attachments may explicitly load instead of clear when a post-process overlays an existing target.

**Pass entry / Prologue**:
The frame packet's unit: a pass plus its destination-only transition set, produced by the RenderGraph from the pass's declared accesses (ADR 05). A transition is a view plus a stage mask — a state requirement, not a barrier.

**Frame completion**:
Backend-independent token returned at end-of-frame; the owning frame slot retains its render result until the token confirms GPU completion (ADR 03).

**Frame-affined task**:
A task-graph callback tagged with the producer thread's frame ordinal; the RHI queue executes only callbacks matching the current frame ordinal. Uploads and acceleration-structure builds ride this transport, so one producer frame's writes land atomically.

**Shader parameters**:
Copyable CPU-side values partitioned by reflected descriptor-set layout; the snapshot carries ref-backed textures, samplers, and buffers.

**Present**:
The explicit blit-to-swapchain command issued by a transfer pass, always blitting the complete source target into the destination viewport with a fixed nearest filter. There is no present flag on passes, views, or usage bits (ADR 05).
_Avoid_: implicit present, present pass flag, present layout at any layer above the backend.

### Synchronization and lifetime

**Barrier diff tracker**:
The backend's emission-side, single-writer state cache. A prologue entry whose desired state differs from the tracked state materializes exactly one native barrier; an equal state emits nothing. Update sources are restricted to prologue transitions and backend-reserved sites (swapchain acquire/present, texture uploads, host belt writes, acceleration-structure builds) — every resource state change flows through it, which is what makes imported-resource prologues correct.

**Deferred deletion queue**:
The module-level, thread-safe queue of native destruction callbacks — the cross-thread transport hop. Last-ref release may enqueue from any thread; the RHI loop drains it on the RHI thread once per frame after the backend tick, with a final flush at backend shutdown. It is valid only between render-device creation and destruction, so no RHIRef may outlive the device (ADR 03).
