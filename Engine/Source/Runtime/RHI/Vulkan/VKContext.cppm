/// @file   Vulkan/VKContext.cppm
/// @brief  Non-owning Vulkan service views used by resource factories.

module;

#include <vk_mem_alloc.h>

export module Vulkan:Context;

import vulkan;

import :DeletionQueue;
import :ImmediateContext;

export namespace SoulEngine {

/// Borrowed device-level services required to create and retire GPU resources.
struct VulkanResourceContext {
    VulkanResourceContext(vk::raii::Device&       InDevice,
                          VmaAllocator            InAllocator,
                          VulkanDeletionQueue&    InDeletionQueue,
                          VulkanImmediateContext& InImmediate,
                          Uint32                  InGraphicsFamily,
                          Uint32                  InTransferFamily)
        : Device(InDevice),
          Allocator(InAllocator),
          DeletionQueue(InDeletionQueue),
          Immediate(InImmediate),
          GraphicsFamily(InGraphicsFamily),
          TransferFamily(InTransferFamily) {}

    vk::raii::Device&       Device;
    VmaAllocator            Allocator;
    VulkanDeletionQueue&    DeletionQueue;
    VulkanImmediateContext& Immediate;
    Uint32                  GraphicsFamily = vk::QueueFamilyIgnored;
    Uint32                  TransferFamily = vk::QueueFamilyIgnored;
};

} // namespace SoulEngine
