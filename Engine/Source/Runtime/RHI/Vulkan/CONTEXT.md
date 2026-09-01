# Context: Vulkan

**Namespace:** SoulEngine

Vulkan RHI backend. It is the current concrete implementation of the
FrameSlot-owned RHIRef contract described by the RHI context.

## Terms

| Term | Definition |
|------|------------|
| **Graphics timeline** | Device-level timeline semaphore signalled by every frame submission. RHIFrameCompletion stores the corresponding completion value. |
| **Frame completion** | Backend-independent token returned by EndFrame(); RenderThread waits on it before replacing the slot-owned RenderResult. |
| **BeginFrame** | Waits for reuse of the backend frame context and resets frame-local scratch/descriptor/transient arenas. |
| **ImmediateContext** | Unified one-shot executor with transfer and graphics lanes. Each lane has a queue-local timeline and ordered completion callbacks. Tick() polls both lanes; Drain() waits and retires them at shutdown. |
| **Immediate completion callback** | Callback retained by an immediate-lane timeline point. Async buffer/texture creation captures the RHI ref payload and marks it ready only after the required transfer/graphics chain completes. |
| **Deferred deletion queue** | RHI-module-owned queue behind GDeferredDeletionQueue. Final RHIRef release enqueues native destruction; RHILoop drains it on the RHI thread after Tick() via DrainRHIDeferredDeletions(), and Shutdown() performs the final drain. |
| **Transient arena** | Host-visible per-frame uniform or shader-storage backing buffer. The current arena segment is reused only after the matching frame-context timeline wait. |
| **Swapchain image** | Backend-private image acquired for presentation. PresentSourceRef is transitioned/copied or rendered into it; it is not a Resource payload. |
| **Vulkan object naming** | Application-defined `VK_EXT_debug_utils` labels assigned through `VulkanDebugUtils::SetObjectName`. Every Vulkan handle created or allocated and owned by SoulEngine, including internal handles, is required to receive a deterministic name after successful creation. |

## Debug utilities and object naming

`VulkanDebugUtils` is the only supported path for assigning
`VK_EXT_debug_utils` object names. Callers must not invoke
`setDebugUtilsObjectNameEXT` directly. The service intentionally becomes a
no-op when debug utils are disabled or unavailable, when the name is empty, or
when the handle is null; these cases do not require caller-side branching.

Every Vulkan handle created or allocated by the backend and retained by
SoulEngine must be named immediately after successful creation, before the
handle is moved into its owning object. This includes persistent RHI resources,
their backing and auxiliary objects, and internal objects such as frame
contexts, descriptor resources, swapchain resources, shader modules, and
immediate-context resources. Handles borrowed from an external owner are not
owned creation sites and are not renamed by this contract.

Persistent RHI resources use their logical RHI name as the base name. Derived
native objects use the following grammar:

- `Base` identifies the primary native object.
- `Base#Role` identifies backing or auxiliary storage, such as an image,
  staging buffer, scratch buffer, or shader binding table.
- `Base::Role` identifies related Vulkan state, such as a pipeline layout or
  descriptor-set layout.
- `Internal/<Type>/<Role>/<Instance>` identifies RHI-internal objects, with
  the Vulkan object type immediately after `Internal/`.
- `<Usage>/<Type>/<LogicalPath>` identifies RHI resources visible outside the
  Vulkan backend.

Examples:

```text
Internal/Instance
Internal/Device
Internal/CommandBuffer/Primary/Frame0
Internal/CommandPool/Secondary/Frame0
Camera/RenderTarget/EditorViewport/SceneColor
Renderer/GraphicsPipeline/GeometryPass
```

Names must be deterministic, non-empty, and describe the object's role. New
creation sites must follow this grammar instead of introducing ad hoc
delimiters.

The current implementation does not yet satisfy this contract at every
creation site. Known follow-up coverage includes swapchain image views and
semaphores, immediate-context command pools and command buffers, shader
modules, and descriptor-pool/set objects. Until those sites are updated, this
document describes the required target behavior rather than claiming complete
runtime coverage.

## Frame submission lifetime

VulkanRenderDevice::Execute() validates and records a ref-backed command list
borrowed from the producer FrameSlot. EndFrame() submits the primary command
buffer and returns an RHIFrameCompletion token; it does not retain or move the
list. RenderThread waits on that token immediately before replacing the slot's
RenderResult, so the list's RHIRefs remain alive while the submission is in
flight.

TLAS backing capacity is fixed after creation for now. An update that exceeds
the initial instance capacity fails instead of replacing native backing while
an earlier submission may still reference it.

## Threading and lifecycle

- Normal runtime native creation is enqueued to ThreadQueue::RHI by
  EnqueueResourceCreation; RHILoop drains that queue before Tick() and
  Execute().
- VulkanImmediateContext, command pools, descriptors, frame arenas, and queue
  submission are RHI-thread confined during normal operation. WaitFinish() is
  the explicit exception: it performs only a read-only host wait on the device
  timeline and is callable from RenderThread.
- Initialization and shutdown remain main-thread lifecycle exceptions today.
  Shutdown() waits idle, drains immediate callbacks, drains deferred deletion,
  then releases Vulkan contexts. No RHIRef may outlive this sequence.
- The ImGui overlay command contains borrowed ImGui data valid only through
  Execute(); it is not retained by the backend after submission. Dear ImGui
  owns its own texture lifetime.

## Relevant partitions

| Partition | Responsibility |
|-----------|----------------|
| :RenderDevice | Device bootstrap, frame execution, timeline retirement, resource-creation task enqueueing, shutdown |
| :Command | RHICommand visitor and Vulkan command recording from ready RHIRef payloads |
| :ImmediateContext | Transfer/graphics immediate submissions, cross-lane bridges, timeline callbacks |
| :Buffer, :Texture, :Pipeline, :RayTracingPipeline, :AccelerationStructure | Native payload implementations created and ultimately destroyed on the RHI path |
| :Descriptor, :Swapchain, :Semaphore, :Types | Backend descriptor, presentation, synchronization, and conversion/state support |

## Guardrails

- Keep FrameSlot.RenderPacket alive until both RHI CPU consumption and its
  RHIFrameCompletion timeline have completed; replacement is the retirement
  boundary.
- Do not enqueue a native destructor directly from a Game or Render thread. Use
  RHIRef final release and the deferred-deletion queue.
- Do not reuse transient arena memory or a Vulkan frame context before its
  timeline wait completes.
- Do not add a second backend without an equivalent RHIFrameCompletion wait
  implementation that is safe to call from RenderThread.
