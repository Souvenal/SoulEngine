# Context: Vulkan

**Namespace:** `SoulEngine::RHI::Vulkan`

Vulkan RHI backend — implements `SoulEngine::RHI::RenderDevice`.

## Terms

| Term | Definition |
|------|------------|
| **HostBuffer** | Internal Vulkan buffer with VMA allocation marked mappable (`HOST_ACCESS_SEQUENTIAL_WRITE_BIT`). Exposes `Upload(Data, Size, Offset)` for map+memcpy+unmap, and `DeferredDelete(Queue, Token)` to defer VMA destruction to a transfer completion token. Used as staging buffer for device-local transfers. Created with `BufferUsage::TransferSrc`. |
| **DeviceBuffer** | Internal Vulkan buffer with device-local VMA allocation (`VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE`). Exposes transfer-copy helpers for whole-buffer staging upload. Does NOT host-mappable. Created with `BufferUsage::TransferDst` combined with semantic usage (e.g. `VertexBuffer`, `IndexBuffer`). |
| **ImmediateContext** | One-shot async executor for GPU commands like staging copies. Allocates transient command buffers, submits to the dedicated transfer queue with timeline semaphore signalling, and does not call `waitIdle`. |
| **TransferCompletionQueue** | Class in `:TransferCompletionQueue`. Owns the transfer upload timeline semaphore, allocates upload completion tokens, checks them non-blockingly, and runs callbacks after transfer completion. Deferred deletion is one callback use-case, not the queue's whole responsibility. |

| Term | Definition |
|------|------------|
| **DescriptorManager** | Class in `:Descriptor` partition. Owns descriptor pool + 3 set layouts (Set 0: per-frame UBO; Set 1: immutable samplers; Set 2: bindless SampledImage variable array) + shared pipeline layout. Exposes `BindTo(CommandBuffer&)` for `CommandList::Begin()`, `GetSetLayouts()` for pipeline creation, and `AllocateTexture()`/`WriteTextureSlot()` for texture slot management. No buffer bindless — future buffer access via BDA. |
| **Explicit vertex input layout** | `GraphicsPipelineDesc::VertexInputLayout` is lowered to Vulkan binding and attribute descriptions. Shader reflection validates location/format compatibility with warnings only; it does not define CPU stride or offset. |
| **VertexBinding slot** | Currently one explicit binding, single interleaved buffer, per-vertex rate. Multi-binding and per-instance rate deferred. |
| **TimelineSemaphore** | Wrapper in `:Semaphore` partition around a single per-device VkSemaphore (VK_SEMAPHORE_TYPE_TIMELINE). Owns the monotonic CPU signal counter. Exposes `NextValue()` to allocate the next signal value, `Wait(Value)` for blocking CPU sync, `GetCurrentValue()` for non-blocking GPU-side query. |
| **NextValue** | Method on `TimelineSemaphore`. Atomically increments the internal counter and returns the new value. Callers pass this value to queue submit as the timeline semaphore signal value, then retain it for later completion checking. |
| **Wait(Value)** | Method on `TimelineSemaphore`. Blocks the CPU via `vkWaitSemaphores` until the semaphore reaches at least `Value`. Default timeout is `UINT64_MAX`. |
| **GetCurrentValue** | Method on `TimelineSemaphore`. Returns the semaphore's current GPU-side counter value via `vkGetSemaphoreCounterValue`. Non-blocking. |
| **SubmissionCompleteTimelineValue** | Field on `FrameContext`. Records the timeline value signalled after submitting that frame's command buffers. Used by `BeginFrame` to wait until the previous owner of this frame slot completes on GPU. |
| **GetSignalSubmitInfo(Stage)** | Method on `TimelineSemaphore`. Calls `NextValue()` and wraps the result into a `vk::SemaphoreSubmitInfo` with the given stage mask. Convenience for `EndFrame`. |

## Descriptor Model

The backend uses a simple 3-set descriptor model shared by all pipelines:

| Set | Content | Binding Flags |
|-----|---------|--------------|
| 0   | per-frame UniformBuffer (single descriptor, updated each frame) | (none — fixed) |
| 1   | Immutable samplers | (none — fixed) |
| 2   | Bindless SampledImage array | UPDATE_AFTER_BIND \| PARTIALLY_BOUND \| VARIABLE_DESCRIPTOR_COUNT |

No sampler bindless or buffer bindless. GPU buffer data access is planned via BDA (Buffer Device Address) rather than descriptor tables.

## Partitions

| Partition | File | Role |
|-----------|------|------|
| :Types | VKTypes.cppm | Format conversions, `BufferState`/`ImageState` barrier tracking structs |
| :Swapchain | VKSwapchain.cppm | Swapchain lifecycle |
| :RenderDevice | VKRenderDevice.cppm | Device init, resource creation facades |
| :CommandList | VKCommandList.cppm | Command recording and barriers |
| :Shader | VKShader.cppm | Stage conversion, shader module creation, explicit vertex input lowering |
| :Pipeline | VKPipeline.cppm | Graphics pipeline creation |
| :Capability | VKCapability.cppm | Extension + feature capability declaration, resolution (extension availability + feature pNext chain assembly), and reflection. Owns the device feature chain for the device's lifetime. |
| :Buffer | VKBuffer.cppm | `HostBuffer` (mappable staging), `DeviceBuffer` (device-local), `VertexBuffer`, `IndexBuffer` — VMA-backed buffer classes with Create factories per ADR 02 |
| :VertexBuffer | VKVertexBuffer.cppm | (unused — logic consolidated into :Buffer) |
| :IndexBuffer | VKIndexBuffer.cppm | (unused — logic consolidated into :Buffer) |
| :ImmediateContext | VKImmediateContext.cppm | One-shot async GPU command executor for staging uploads, initial barriers, etc. Transient command buffers, no waitIdle. |
| :TransferCompletionQueue | VKTransferCompletionQueue.cppm | `TransferCompletionQueue` — transfer timeline, upload completion token allocation/query, and deferred callback execution |
| :Descriptor | VKDescriptor.cppm | `DescriptorManager` — pool, 3-set layout, pipeline layout, BindTo, texture slot management |
