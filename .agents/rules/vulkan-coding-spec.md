---
paths:
  - "Engine/Source/Runtime/RHI/Vulkan/**/*.cpp"
  - "Engine/Source/Runtime/RHI/Vulkan/**/*.cppm"
---

# Vulkan Coding Specifications

## pNext

Never set `.pNext` directly on a Vulkan structure. Use `vk::StructureChain` instead to chain extension structs. This guarantees correct type-safe chaining, proper alignment, and automatic cleanup.

### Wrong:
```cpp
vk::SemaphoreCreateInfo SemInfo{};
SemInfo.pNext = &TimelineCI;
```

### Right:
```cpp
vk::StructureChain<vk::SemaphoreCreateInfo, vk::SemaphoreTypeCreateInfo> Chain = {
    {}, {.semaphoreType = vk::SemaphoreType::eTimeline, .initialValue = 0}
};
Device.createSemaphore(Chain.get<vk::SemaphoreCreateInfo>());
```

## No fences — use timeline semaphores

Fences (`vk::Fence`) are banned. Timeline semaphores (`vk::SemaphoreType::eTimeline`) replace them in every role: GPU-GPU ordering, CPU-GPU sync, and blocking wait. `vkGetSemaphoreCounterValue` provides non-blocking completion queries that fences cannot. The one exception: when a third-party library (VMA, swapchain integration) requires a fence parameter at the ABI boundary, use a raw `VkFence` scoped to that single call.

## Variable naming: `*CI` suffix for CreateInfo local variables

Local variables of Vulkan CreateInfo types (any struct whose name ends in `CreateInfo`) must use a **CI** suffix. The prefix describes the object being created or the purpose.

### Examples:

```cpp
vk::SwapchainCreateInfoKHR SwapchainCI;
vk::CommandPoolCreateInfo  PoolCI;
vk::PipelineLayoutCreateInfo TempLayoutCI;
vk::DeviceCreateInfo       DevCI;
auto                       VertexInputCI = ShaderStates->GetPipelineVertexInputStateCI();
```

The suffix keeps variable names compact while making it immediately obvious that the variable is a CreateInfo struct, not the resulting object.

## Object naming

Every Vulkan handle created or allocated by SoulEngine and retained by the
backend must receive a deterministic `VK_EXT_debug_utils` name immediately
after successful creation, before ownership is moved. This requirement applies
to public RHI resources, native backing objects, and internal objects such as
frame resources, swapchain resources, shader modules, descriptor resources,
and immediate-context resources.

Use `VulkanDebugUtils::SetObjectName` for all object naming. Do not call
`setDebugUtilsObjectNameEXT` directly outside `VKDebug.cppm`. The helper
already handles disabled or unavailable debug utils, empty names, null handles,
and reports naming failures.

Use the following grammar for deterministic object names:

```text
Internal/<Type>/<Role>/<Instance>
<Usage>/<Type>/<LogicalPath>
<Base>#<NativeRole>
<Base>::<RelatedRole>
```

RHI-internal objects use the `Internal/` namespace. The Vulkan object type is
the first path component after `Internal/`; roles and instance identifiers
follow it. Use compact stable instance identifiers such as `Frame0` and
`Swapchain0`, not bracketed or nested index syntax.

Examples:

```text
Internal/Instance
Internal/Surface
Internal/PhysicalDevice
Internal/Device
Internal/Queue/Graphics
Internal/CommandBuffer/Primary/Frame0
Internal/CommandPool/Secondary/Frame0
Internal/ImageView/Swapchain0
```

RHI resources visible outside the Vulkan backend use an ownership-oriented
usage path, for example `Camera/RenderTarget/EditorViewport/SceneColor` or
`Renderer/GraphicsPipeline/GeometryPass`.

Native backing objects append `#<NativeRole>` to the owning RHI resource
name. Related Vulkan state objects append `::<RelatedRole>`. Names must be
stable, non-empty, and role-specific. Handles borrowed from an external owner
are not renamed by this rule.

## No C-style Vulkan (except VMA)

Prefer `vk::raii::*` types and C++ Vulkan-Hpp wrappers. Raw C Vulkan types (`VkBuffer`, `VkDevice`, `VkCommandBuffer`, etc.) and raw C API calls (`vkFreeCommandBuffers`, `vkDestroy*`, etc.) are banned.

The only exception is VMA interop code — `reinterpret_cast` between `VkBuffer`/`VkDevice`/`VmaAllocation` and their C++ counterparts is permitted because VMA is a C library at the ABI boundary. All other code must use `vk::*` / `vk::raii::*`.
