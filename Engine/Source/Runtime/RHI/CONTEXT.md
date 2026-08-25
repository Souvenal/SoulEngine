# Context: RHI

**Namespace:** SoulEngine

Render Hardware Interface — abstract GPU layer. Backends register through the
self-registering factory. The RHI boundary owns native-resource construction,
submission lifetime, and native destruction; higher layers carry RHIRef<T>
handles and record declarative commands.

## Terms

| Term | Definition |
|------|------------|
| **RHIRef<T>** | Copyable handle to a shared RHIRefPayload<T>. It represents a particular backend-native resource without exposing ownership of the native T. States are RhiCommitting, GpuPending, Ready, and Failed. Its explicit `operator bool()` is true only when the payload exists and is Ready; `operator->`/`operator*` provide access after that guard. `TryGet()` remains the explicit nullable access path for backend lowering. |
| **RHIRef payload** | Shared state containing the atomic availability state, optional error, and the native UPtr<T>. Its final destruction moves the native object into the deferred-deletion queue rather than destroying it on the releasing thread. |
| **Deferred deletion queue** | Thread-safe queue of move-only destruction callbacks owned by RHIRenderDevice. Last-ref release may enqueue from any thread; RHIRenderDevice::Tick() drains it on the RHI thread. It is valid only between RHIRenderDevice::Create() and Destroy(). |
| **CommandList** | Move-only frame packet containing ordered rendering/non-rendering scopes, a final PresentSourceRef, and optionally an ImGui presentation overlay. Every resource-bearing command field uses RHIRef<T> rather than an owning or observer pointer. |
| **Pass / scope** | A rendering RHIPass or a RHINonRenderingPass containing declarative RHICommand work. One RHIPass owns an ordered color-attachment list plus an optional depth attachment and may bind multiple compatible graphics pipelines. Attachments, pipelines, draw buffers, shader-parameter resources, TLAS/BLAS instances, and presentation source are ref-backed. Color and depth attachments can explicitly load instead of clear when a post-process overlays an existing target. |
| **In-flight submission** | Backend-owned retention record for a submitted command list and auxiliary retirement callbacks. Vulkan retains this record until its graphics timeline reaches the submission value. |
| **Shader parameters** | Copyable CPU-side values partitioned by reflected descriptor-set layout. RHIShaderParameterResources carries ref-backed sampled textures, samplers, and render targets used by that snapshot. |
| **Transient buffers and arenas** | RenderDevice creates typed, immediately-ready `RHIRef` transient uniform/storage-buffer resources from data descriptors. The RHI frame task copies data into the current backend frame arena after resetting that frame's region and resolves the backend slice used by shader binding. A transient ref is valid only for the logical frame in which it was created. |
| **RenderDevice** | Process-wide RHI singleton. Its create APIs return RHIRef<T> immediately and queue backend-native construction to ThreadQueue::RHI; Execute() consumes a command list; Tick() retires backend completions then deferred destruction. |
| **Ready resource** | A ref for which `operator bool()` is true and whose payload can be read through `operator->`, `operator*`, or `TryGet()`. GPU-uploaded buffers/textures become ready only when their immediate-context completion callback retires. |
| **Render target / present source** | Engine-owned ref-backed attachment image. PresentSourceRef is the final color output; the backend copies/blits/renders it into a backend-private swapchain image. |
| **Swapchain image** | Backend-private presentation image; never a Resource-managed sampled texture or an RHIRef exposed to Renderer. |
| **Graphics / ray-tracing pipeline** | Backend-polymorphic pipeline payload retained by bind, draw, push-constant, parameter-binding, or trace commands. |
| **Frame-affined task** | A TaskGraph callback tagged with the producer thread's frame ordinal. The RHI queue executes only callbacks matching the current RHI thread ordinal. |
| **Transient data upload** | A transient RHI resource creation task owns a byte snapshot, allocates the current Vulkan arena after frame reset, writes the data, and publishes a Ready `RHIRef` before command recording. |
| **Buffer device address** | `RHIVertexBuffer::GetDeviceAddress()` and `RHIIndexBuffer::GetDeviceAddress()` expose the address value used by current shader ABI records. Vulkan implements these values with BDA. |

## Submission and destruction contract

1. A caller asks RHIRenderDevice to create a resource and receives an RHIRef.
   The Vulkan backend queues the native creation closure on ThreadQueue::RHI.
2. Render code records ready refs. Transient refs are immediately ready logical
   frame resources; their RHI frame task resolves the arena slice before Vulkan
   command recording. The command list takes copies/moves of the refs, so
   command recording does not depend on raw RHI observers.
3. Vulkan records and submits the list, then moves the complete list into
   m_InFlightSubmissions with the graphics timeline value signalled by that
   submission. RetireInFlightSubmissions() releases it only after the timeline
   reports completion.
4. Releasing the last RHIRef moves the native object into the render-device
   deletion queue. The queue is drained by Tick() on the RHI thread. For a
   submission-held ref, this cannot happen until the submission is complete.

The command-list lifetime is therefore **submission-owned in the Vulkan
backend**. The FrameSlot itself is not the final owner after Execute():
Execute(std::move(Slot.RenderPacket.CmdList)) transfers the list to the
backend and RHILoop clears the now-moved-from slot packet before publishing
RHIDone. This is safe for Vulkan because the in-flight submission record
retains the refs, but it is not the originally requested policy of clearing the
list only when GameLoop reacquires the slot.

RHIImGuiPresentationOverlayCmd is the current exception to ref-backed command
payloads: it contains non-owning ImGui snapshot/texture-queue/mutex pointers
that need to remain valid only through Execute(). Its backend texture objects
are managed by Dear ImGui rather than by RHIRef; do not extend this exception
to ordinary RHI resources.

## Thread model

| Operation | Required thread / current behavior |
|-----------|------------------------------------|
| CPU request preparation and RHIRef state reads | Any producer/consumer thread. `operator bool()` is the Ready guard; `TryGet()` and `operator->` are state-gated reads, not permission to mutate native state. |
| Native resource creation, upload submission, completion publication, Execute(), Tick(), and deferred deletion drain | RHI thread during normal runtime. |
| RHIRef last-release | Any thread; it only enqueues the native destructor. |
| Device initialization and final shutdown | Main thread today: RHIRenderDevice::Create() runs during EngineLoop::Init(), and Destroy()/backend Shutdown() run after the RHI thread joins. These are explicit lifecycle exceptions, so the code does **not** yet enforce a strict all-RHI-context-calls-on-RHI-thread rule. |

## Relationships

- RHI does not import Resource and does not use ResourceHandle<T>.
- Resource, Scene, and Renderer may own/ref-count higher-level request objects;
  a recorded command owns the RHI-lifetime portion through RHIRef<T>.
- A backend that implements Execute() must retain every ref-backed command
  resource until that submission is no longer GPU-visible. This is currently
  implemented by Vulkan but is not expressed as an interface-level requirement
  or test shared by all future backends.
- Resource readiness is separate from submission lifetime: GpuPending means
  a newly created payload is not recordable; an in-flight submitted Ready
  payload remains alive through command-list retention.
- Frame-affined transient creation tasks retain byte snapshots until the RHI
  thread resets the current arena and allocates/writes the payload. The transient
  ref is created Ready, but it cannot be retained or rebound after its creation
  frame. The command list retains the ref while the backend records and submits
  that frame.

## Transient data upload

Uploadable records define a nested `GpuData` ABI and a `BuildGpuData()` member
that constructs it directly from their RHI buffer members and scalar fields.
Renderer code creates a transient buffer from the resulting byte snapshot and
stores the immediately-ready `RHIRef` in shader parameters or commands. The RHI
frame task allocates and writes the current backend arena after its frame region
has been reset, then fills the backend transient object's arena offset. Vulkan
binds that offset directly and emits one
aggregated host-write visibility barrier for UniformRead, ShaderRead, and
IndirectCommandRead consumers.

## Current gaps / guardrails

- GDeferredDeletionQueue is a process-wide raw pointer. A payload whose last
  ref is released after RHIRenderDevice::Destroy() would dereference a null
  queue; shutdown order must release all refs first.
- Tick() and deletion draining have a documented RHI-thread precondition but
  no runtime thread-affinity assertion.
- The FrameSlot reset point and the backend submission-retention point are
  deliberately different today. Do not claim slot reuse itself proves GPU
  completion.
- The generic RHI interface does not yet expose a backend-independent submission
  retirement contract; a new backend must provide one before using RHIRef.
- Current-frame CPU-to-GPU uploads belong in frame-affined transient creation
  tasks, not individual RHICommand upload variants or scopes. New uploadable
  data must provide a GPU ABI record and materialize its bytes before calling
  CreateTransientXXXBuffer().

## Dependencies

- Core — common types, errors, logging, factory, task graph access
- WindowSystem — backend presentation-surface bootstrap
- Backend modules — self-register through RHIBackendFactory; RHI itself never imports a concrete backend
