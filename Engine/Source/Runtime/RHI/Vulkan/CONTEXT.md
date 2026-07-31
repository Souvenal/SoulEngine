# Context: Vulkan

**Namespace:** SoulEngine

Vulkan RHI backend. It is the current concrete implementation of the
submission-owned RHIRef contract described by the RHI context.

## Terms

| Term | Definition |
|------|------------|
| **Graphics timeline** | Device-level timeline semaphore signalled by every frame submission. A VulkanInFlightSubmission stores the corresponding completion value. |
| **VulkanInFlightSubmission** | Queue entry containing the submitted RHICommandList and auxiliary RetiredPayloads. Its destruction releases command-held RHIRef values only after the graphics timeline reaches its completion value. |
| **BeginFrame** | Waits for reuse of the backend frame context, resets frame-local scratch/descriptor/transient arenas, and calls RetireInFlightSubmissions() after the timeline observation. |
| **ImmediateContext** | Unified one-shot executor with transfer and graphics lanes. Each lane has a queue-local timeline and ordered completion callbacks. Tick() polls both lanes; Drain() waits and retires them at shutdown. |
| **Immediate completion callback** | Callback retained by an immediate-lane timeline point. Async buffer/texture creation captures the RHI ref payload and marks it ready only after the required transfer/graphics chain completes. |
| **Retired payload** | Auxiliary native object, for example replaced TLAS backing storage, retained beside the frame submission until that submission retires. |
| **Deferred deletion queue** | Base render-device queue. Final RHIRef release enqueues native destruction; normal-runtime RHIRenderDevice::Tick() drains it on the RHI thread. |
| **Transient arena** | Host-visible per-frame uniform or shader-storage backing buffer. The current arena segment is reused only after the matching frame-context timeline wait. |
| **Swapchain image** | Backend-private image acquired for presentation. PresentSourceRef is transitioned/copied or rendered into it; it is not a Resource payload. |

## Frame submission lifetime

VulkanRenderDevice::Execute() validates and records a ref-backed command list,
signals the graphics timeline in the primary queue submission, and moves the
whole list into m_InFlightSubmissions. The producer FrameSlot may therefore
be cleared immediately after Execute() returns: the backend, not the slot,
retains every ordinary command ref while the GPU can access it.

At a later BeginFrame(), Vulkan waits for the reused backend frame context,
polls the graphics timeline, and pops only submissions whose completion value
has been reached. Popping destroys the retained command list and may enqueue
last-ref native destruction. The next RHI-thread Tick() drains that queue.
This ordering prevents a normal ref-backed frame resource from being destroyed
while its Vulkan submission remains in flight.

RetiredPayloads follows the same submission timeline. It covers native
scratch and old backing allocations used by a recorded TLAS update and
supplements RHIRef retention.

## Threading and lifecycle

- Normal runtime native creation is enqueued to ThreadQueue::RHI by
  EnqueueResourceCreation; RHILoop drains that queue before Tick() and
  Execute().
- VulkanImmediateContext, command pools, descriptors, frame arenas, and queue
  submission are RHI-thread confined during normal operation.
- Initialization and shutdown remain main-thread lifecycle exceptions today.
  Shutdown() waits idle, drains immediate callbacks, clears in-flight
  submissions, drains deferred deletion, then releases Vulkan contexts. No
  RHIRef may outlive this sequence.
- The ImGui overlay command contains borrowed ImGui data valid only through
  Execute(); it is not placed in VulkanInFlightSubmission for post-submit
  use. Dear ImGui owns its own texture lifetime.

## Relevant partitions

| Partition | Responsibility |
|-----------|----------------|
| :RenderDevice | Device bootstrap, frame execution, timeline retirement, resource-creation task enqueueing, shutdown |
| :Command | RHICommand visitor and Vulkan command recording from ready RHIRef payloads |
| :ImmediateContext | Transfer/graphics immediate submissions, cross-lane bridges, timeline callbacks |
| :Buffer, :Texture, :Pipeline, :RayTracingPipeline, :AccelerationStructure | Native payload implementations created and ultimately destroyed on the RHI path |
| :Descriptor, :Swapchain, :Semaphore, :Types | Backend descriptor, presentation, synchronization, and conversion/state support |

## Guardrails

- Do not remove the command-list move into m_InFlightSubmissions merely
  because FrameSlot becomes reusable; those events are not equivalent.
- Do not enqueue a native destructor directly from a Game or Render thread. Use
  RHIRef final release and the deferred-deletion queue.
- Do not reuse transient arena memory or a Vulkan frame context before its
  timeline wait completes.
- Do not add a second backend without an equivalent submission-retention rule.