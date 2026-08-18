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

Use the owning RHI object's logical name as the base name. Derived names must
follow this grammar:

```text
Base                 primary native object
Base#Role            backing or auxiliary object
Base::Role           related Vulkan state
Frame[index]::Role   frame-scoped object
```

Names must be stable, non-empty, and role-specific. Do not introduce ad hoc
delimiters or name only the top-level object when the creation site also owns
backing or related Vulkan objects. Handles borrowed from an external owner are
not renamed by this rule.

## No C-style Vulkan (except VMA)

Prefer `vk::raii::*` types and C++ Vulkan-Hpp wrappers. Raw C Vulkan types (`VkBuffer`, `VkDevice`, `VkCommandBuffer`, etc.) and raw C API calls (`vkFreeCommandBuffers`, `vkDestroy*`, etc.) are banned.

The only exception is VMA interop code — `reinterpret_cast` between `VkBuffer`/`VkDevice`/`VmaAllocation` and their C++ counterparts is permitted because VMA is a C library at the ABI boundary. All other code must use `vk::*` / `vk::raii::*`.
