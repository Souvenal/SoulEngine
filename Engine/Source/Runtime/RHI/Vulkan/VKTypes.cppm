module;

#include <vk_mem_alloc.h>

export module Vulkan:Types;

export import std;
export import vulkan;

import RHI;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {


// ═════════════════════════════════════════════════════════════════════════════
// Conversion helpers
// ═════════════════════════════════════════════════════════════════════════════

[[nodiscard]] auto ToVkFormat(Format Fmt) -> vk::Format {
    switch (Fmt) {
    case Format::R8G8B8A8_UNORM:
        return vk::Format::eR8G8B8A8Unorm;
    case Format::B8G8R8A8_UNORM:
        return vk::Format::eB8G8R8A8Unorm;
    case Format::D32_SFLOAT:
        return vk::Format::eD32Sfloat;
    case Format::D32_SFLOAT_S8_UINT:
        return vk::Format::eD32SfloatS8Uint;
    case Format::R32G32B32A32_SFLOAT:
        return vk::Format::eR32G32B32A32Sfloat;
    case Format::R32G32B32_SFLOAT:
        return vk::Format::eR32G32B32Sfloat;
    case Format::R32G32_SFLOAT:
        return vk::Format::eR32G32Sfloat;
    case Format::R32_SFLOAT:
        return vk::Format::eR32Sfloat;
    default:
        return vk::Format::eUndefined;
    }
}

[[nodiscard]] auto ToVkBufferUsage(BufferUsage Usage) -> vk::BufferUsageFlags {
    vk::BufferUsageFlags Flags;
    if (Usage & BufferUsage::VertexBuffer)
        Flags |= vk::BufferUsageFlagBits::eVertexBuffer;
    if (Usage & BufferUsage::IndexBuffer)
        Flags |= vk::BufferUsageFlagBits::eIndexBuffer;
    if (Usage & BufferUsage::UniformBuffer)
        Flags |= vk::BufferUsageFlagBits::eUniformBuffer;
    if (Usage & BufferUsage::StorageBuffer)
        Flags |= vk::BufferUsageFlagBits::eStorageBuffer;
    if (Usage & BufferUsage::TransferSrc)
        Flags |= vk::BufferUsageFlagBits::eTransferSrc;
    if (Usage & BufferUsage::TransferDst)
        Flags |= vk::BufferUsageFlagBits::eTransferDst;
    if (Usage & BufferUsage::Indirect)
        Flags |= vk::BufferUsageFlagBits::eIndirectBuffer;
    return Flags;
}

// ═════════════════════════════════════════════════════════════════════════════
// Barrier state tracking (Sync2)
// ═════════════════════════════════════════════════════════════════════════════

/// Per-buffer GPU state for automatic barrier generation.
/// Buffers have no image-layout concept; layout tracking is not needed.
struct BufferState {
    vk::PipelineStageFlags2 stage       = vk::PipelineStageFlagBits2::eNone;
    vk::AccessFlags2        access      = vk::AccessFlagBits2::eNone;
    Uint32                  queueFamily = vk::QueueFamilyIgnored;
    bool                    isWrite     = false;
};

/// Per-image GPU state for automatic barrier generation.
/// Default layout = eUndefined so first-use transitions derive the correct
/// srcLayout without special-case init logic.
struct ImageState {
    vk::PipelineStageFlags2 stage       = vk::PipelineStageFlagBits2::eNone;
    vk::AccessFlags2        access      = vk::AccessFlagBits2::eNone;
    vk::ImageLayout         layout      = vk::ImageLayout::eUndefined;
    Uint32                  queueFamily = vk::QueueFamilyIgnored;
    bool                    isWrite     = false;
};

} // namespace SoulEngine::RHI::Vulkan
