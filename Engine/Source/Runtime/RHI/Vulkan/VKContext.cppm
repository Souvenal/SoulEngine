/// @file   Vulkan/VKContext.cppm
/// @brief  Non-owning Vulkan service views used by resource factories.

module;

#include <vk_mem_alloc.h>

export module Vulkan:Context;

import vulkan;

import :Debug;
import :ImmediateContext;

export namespace SoulEngine {

/// Borrowed device-level services required to create and retire GPU resources.
struct VulkanResourceContext {
    VulkanResourceContext(vk::raii::Device&       InDevice,
                          VulkanDebugUtils&       InDebugUtils,
                          VmaAllocator            InAllocator,
                          VulkanImmediateContext& InImmediate,
                          Uint32                  InGraphicsFamily,
                          Uint32                  InTransferFamily)
        : Device(InDevice),
          DebugUtils(InDebugUtils),
          Allocator(InAllocator),
          Immediate(InImmediate),
          GraphicsFamily(InGraphicsFamily),
          TransferFamily(InTransferFamily) {}

    vk::raii::Device&       Device;
    VulkanDebugUtils&       DebugUtils;
    VmaAllocator            Allocator;
    VulkanImmediateContext& Immediate;
    Uint32                  GraphicsFamily = vk::QueueFamilyIgnored;
    Uint32                  TransferFamily = vk::QueueFamilyIgnored;
};

} // namespace SoulEngine
