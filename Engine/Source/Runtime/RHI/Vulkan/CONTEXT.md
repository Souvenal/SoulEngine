# Context: Vulkan

**Namespace:** `SoulEngine::RHI::Vulkan`

Vulkan RHI backend — implements `SoulEngine::RHI::RenderDevice`.

## Terms

| Term | Definition |
|------|------------|
| **HostBuffer** | Internal Vulkan buffer with VMA allocation marked mappable (`HOST_ACCESS_SEQUENTIAL_WRITE_BIT`). Exposes `Upload(Data, Size, Offset)` for map+memcpy+unmap, and `DeferredDelete(Queue, Value)` to defer VMA destruction to a timeline value. Used as staging buffer for device-local transfers. Created with `BufferUsage::TransferSrc`. |
| **DeviceBuffer** | Internal Vulkan buffer with device-local VMA allocation (`VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE`). Exposes `CopyFrom(HostBuffer&, ImmediateContext&, SignalSema)` for whole-buffer transfer with async completion signalling. Does NOT host-mappable. Created with `BufferUsage::TransferDst` combined with semantic usage (e.g. `VertexBuffer`, `IndexBuffer`). |
| **ImmediateContext** | One-shot async executor for GPU commands like staging copies. Uses persistent command buffer with pre-submit lightweight wait on previous completion. Submits to transfer queue with a timeline semaphore signal; no `waitIdle`. |

| Term | Definition |
|------|------------|
| **DescriptorManager** | Class in `:Descriptor` partition. Owns descriptor pool + 2 set layouts (Set 0: per-frame UBO; Set 1: bindless SampledImage variable array) + shared pipeline layout. Exposes `BindTo(CommandBuffer&)` for `CommandList::Begin()`, `GetSetLayouts()` for pipeline creation, and `AllocateTexture()`/`WriteTextureSlot()` for texture slot management. No sampler or buffer bindless — future buffer access via BDA. |
| **CalculateStride** | Helper in `:Shader` partition. Derives vertex attribute byte stride from Shader::ValueType (columnCount × 4). Float32/Int32/Uint32 only. |
| **VertexBinding slot** | Currently hardcoded to binding 0, single interleaved buffer, per-vertex rate. Multi-binding and per-instance rate deferred. |
| **TimelineSemaphore** | Wrapper in `:Semaphore` partition around a single per-device VkSemaphore (VK_SEMAPHORE_TYPE_TIMELINE). Owns the monotonic CPU signal counter. Exposes `NextValue()` to allocate the next signal value, `Wait(Value)` for blocking CPU sync, `GetCurrentValue()` for non-blocking GPU-side query. |
| **NextValue** | Method on `TimelineSemaphore`. Atomically increments the internal counter and returns the new value. Callers pass this value to queue submit as the timeline semaphore signal value, then retain it for later completion checking. |
| **Wait(Value)** | Method on `TimelineSemaphore`. Blocks the CPU via `vkWaitSemaphores` until the semaphore reaches at least `Value`. Default timeout is `UINT64_MAX`. |
| **GetCurrentValue** | Method on `TimelineSemaphore`. Returns the semaphore's current GPU-side counter value via `vkGetSemaphoreCounterValue`. Non-blocking. |
| **SubmissionCompleteTimelineValue** | Field on `FrameContext`. Records the timeline value signalled after submitting that frame's command buffers. Used by `BeginFrame` to wait until the previous owner of this frame slot completes on GPU. |
| **GetSignalSubmitInfo(Stage)** | Method on `TimelineSemaphore`. Calls `NextValue()` and wraps the result into a `vk::SemaphoreSubmitInfo` with the given stage mask. Convenience for `EndFrame` and `DeferredDeletionQueue`. |
| **DeferredDeletionQueue** | Class in `:DeletionQueue` partition for deferred GPU resource destruction. Owns a dedicated timeline semaphore. `Enqueue(Value, Fn)` registers a callback; `Tick()` fires all completed callbacks (non-blocking, per‑frame); `Drain()` blocks until all complete. Enforces monotonic ordering via its own semaphore, avoiding cross‑queue ordering constraints. |

## Descriptor Model

The backend uses a simple 2-set descriptor model shared by all pipelines:

| Set | Content | Binding Flags |
|-----|---------|--------------|
| 0   | per-frame UniformBuffer (single descriptor, updated each frame) | (none — fixed) |
| 1   | Bindless SampledImage array | UPDATE_AFTER_BIND \| PARTIALLY_BOUND \| VARIABLE_DESCRIPTOR_COUNT |

No sampler bindless or buffer bindless. Samplers may be added later as a set 2 if needed. GPU buffer data access is planned via BDA (Buffer Device Address) rather than descriptor tables. |

## Partitions

| Partition | File | Role |
|-----------|------|------|
| :Types | VKTypes.cppm | Format conversions, `BufferState`/`ImageState` barrier tracking structs |
| :Swapchain | VKSwapchain.cppm | Swapchain lifecycle |
| :RenderDevice | VKRenderDevice.cppm | Device init, resource creation facades |
| :CommandList | VKCommandList.cppm | Command recording and barriers |
| :Shader | VKShader.cppm | Stage conversion, shader module creation, vertex input derivation |
| :Pipeline | VKPipeline.cppm | Graphics pipeline creation |
| :Capability | VKCapability.cppm | Extension + feature capability declaration, resolution (extension availability + feature pNext chain assembly), and reflection. Owns the device feature chain for the device's lifetime. |
| :Buffer | VKBuffer.cppm | `HostBuffer` (mappable staging), `DeviceBuffer` (device-local), `VertexBuffer`, `IndexBuffer` — VMA-backed buffer classes with Create factories per ADR 02 |
| :VertexBuffer | VKVertexBuffer.cppm | (unused — logic consolidated into :Buffer) |
| :IndexBuffer | VKIndexBuffer.cppm | (unused — logic consolidated into :Buffer) |
| :ImmediateContext | VKImmediateContext.cppm | One-shot async GPU command executor for staging uploads, initial barriers, etc. Persistent command buffer, no waitIdle. |
| :DeletionQueue | VKDeletionQueue.cppm | `DeferredDeletionQueue` — timeline-backed deferred resource destruction |
| :Descriptor | VKDescriptor.cppm | `DescriptorManager` — pool, 2-set layout, pipeline layout, BindTo, texture slot management |
