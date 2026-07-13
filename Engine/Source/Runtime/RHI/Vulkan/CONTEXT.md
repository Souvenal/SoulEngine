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
| **DescriptorLayoutConfig** | Vulkan-internal read-only descriptor ABI config. It carries immutable sampler handles and bindless table limits from `RenderDevice` to both `DescriptorManager` and `Vulkan::GraphicsPipeline`, so pipeline layout generation does not query descriptor allocation state. |
| **DescriptorManager** | Class in `:Descriptor` partition. Owns descriptor pool policy plus long-lived descriptor sets such as the immutable sampler set and the bindless SampledImage table. It allocates/writes descriptor sets, but it does not own a global pipeline layout and does not bind descriptor sets for a command buffer. Pipeline layouts are generated from shader reflection and owned by `Vulkan::GraphicsPipeline`. No buffer bindless — future buffer access via BDA. |
| **Shader-oriented pipeline layout** | Each `Vulkan::GraphicsPipeline` consumes `Shader::GraphicsProgram::Reflection`, creates the matching Vulkan descriptor set layouts and `vk::PipelineLayout`, and keeps reflected binding path lookup data for recording-time descriptor binding. |
| **Explicit vertex input layout** | `GraphicsPipelineDesc::VertexInputLayout` is lowered to Vulkan binding and attribute descriptions. Shader reflection validates location/format compatibility with warnings only; it does not define CPU stride or offset. |
| **VertexBinding slot** | Currently one explicit binding, single interleaved buffer, per-vertex rate. Multi-binding and per-instance rate deferred. |
| **TimelineSemaphore** | Wrapper in `:Semaphore` partition around a single per-device VkSemaphore (VK_SEMAPHORE_TYPE_TIMELINE). Owns the monotonic CPU signal counter. Exposes `NextValue()` to allocate the next signal value, `Wait(Value)` for blocking CPU sync, `GetCurrentValue()` for non-blocking GPU-side query. |
| **NextValue** | Method on `TimelineSemaphore`. Atomically increments the internal counter and returns the new value. Callers pass this value to queue submit as the timeline semaphore signal value, then retain it for later completion checking. |
| **Wait(Value)** | Method on `TimelineSemaphore`. Blocks the CPU via `vkWaitSemaphores` until the semaphore reaches at least `Value`. Default timeout is `UINT64_MAX`. |
| **GetCurrentValue** | Method on `TimelineSemaphore`. Returns the semaphore's current GPU-side counter value via `vkGetSemaphoreCounterValue`. Non-blocking. |
| **SubmissionCompleteTimelineValue** | Field on `FrameContext`. Records the timeline value signalled after submitting that frame's command buffers. Used by `BeginFrame` to wait until the previous owner of this frame slot completes on GPU. |
| **GetSignalSubmitInfo(Stage)** | Method on `TimelineSemaphore`. Calls `NextValue()` and wraps the result into a `vk::SemaphoreSubmitInfo` with the given stage mask. Convenience for `EndFrame`. |

## Descriptor Model

Descriptor set layouts are generated per graphics pipeline from shader
reflection. All uniform buffers are lowered as dynamic uniform-buffer
descriptors backed by the backend constant-buffer upload path. Dynamic offsets
are ordered from the reflected set/binding layout, not hard-coded at call sites.
Descriptor stage visibility is currently lowered conservatively to all graphics
stages so pipeline-owned set layouts remain compatible with the long-lived
descriptor sets allocated by `DescriptorManager`.

The current shader convention still uses three logical groups:

| Set | Content | Binding Flags |
|-----|---------|--------------|
| 0   | Frame UniformBuffers from `g_frame` | (none — fixed) |
| 1   | Immutable samplers | (none — fixed) |
| 2   | Bindless SampledImage array | UPDATE_AFTER_BIND \| PARTIALLY_BOUND \| VARIABLE_DESCRIPTOR_COUNT |

Those set numbers are reflected shader ABI, not public RHI API. No sampler
bindless or buffer bindless. GPU buffer data access is planned via BDA (Buffer
Device Address) rather than descriptor tables.

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
| :Descriptor | VKDescriptor.cppm | `DescriptorManager` — descriptor allocation/write policy, immutable sampler descriptors, bindless texture slot management |
| :Texture | VKTexture.cppm | `DeviceTexture`, `SampledTexture`, and `RenderTarget` image resources |

## Presentation

Normal render passes target engine-owned `RHI::RenderTarget` images. The Vulkan
backend does not treat a null color attachment as the swapchain. At the end of
`RenderDevice::Execute()`, `CommandList::PresentSource` is transitioned to a
backend transfer-source layout, the acquired swapchain image is transitioned to
a backend transfer-destination layout, and the source is blitted into the
swapchain image before presentation. Public RHI callers express this intent
with `TextureUsage::FrameOutput`; they do not request native transfer usage.
