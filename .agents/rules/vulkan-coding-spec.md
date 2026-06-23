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

## No C-style Vulkan (except VMA)

Prefer `vk::raii::*` types and C++ Vulkan-Hpp wrappers. Raw C Vulkan types (`VkBuffer`, `VkDevice`, `VkCommandBuffer`, etc.) and raw C API calls (`vkFreeCommandBuffers`, `vkDestroy*`, etc.) are banned.

The only exception is VMA interop code — `reinterpret_cast` between `VkBuffer`/`VkDevice`/`VmaAllocation` and their C++ counterparts is permitted because VMA is a C library at the ABI boundary. All other code must use `vk::*` / `vk::raii::*`.
