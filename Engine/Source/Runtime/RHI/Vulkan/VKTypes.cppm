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

[[nodiscard]] auto ToVkFormat(RHI::Format Fmt) -> vk::Format {
    switch (Fmt) {
    case RHI::Format::R8G8B8A8_UNORM:
        return vk::Format::eR8G8B8A8Unorm;
    case RHI::Format::B8G8R8A8_UNORM:
        return vk::Format::eB8G8R8A8Unorm;
    case RHI::Format::D32_SFLOAT:
        return vk::Format::eD32Sfloat;
    case RHI::Format::D24_UNORM_S8_UINT:
        return vk::Format::eD24UnormS8Uint;
    case RHI::Format::D32_SFLOAT_S8_UINT:
        return vk::Format::eD32SfloatS8Uint;
    case RHI::Format::R32G32B32A32_SFLOAT:
        return vk::Format::eR32G32B32A32Sfloat;
    case RHI::Format::R32G32B32_SFLOAT:
        return vk::Format::eR32G32B32Sfloat;
    case RHI::Format::R32G32_SFLOAT:
        return vk::Format::eR32G32Sfloat;
    case RHI::Format::R32_SFLOAT:
        return vk::Format::eR32Sfloat;
    default:
        return vk::Format::eUndefined;
    }
}

[[nodiscard]] auto ToVkImageAspect(RHI::Format Fmt) -> vk::ImageAspectFlags {
    switch (Fmt) {
    case RHI::Format::D32_SFLOAT:
        return vk::ImageAspectFlagBits::eDepth;
    case RHI::Format::D32_SFLOAT_S8_UINT:
    case RHI::Format::D24_UNORM_S8_UINT:
        return vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
    default:
        return vk::ImageAspectFlagBits::eColor;
    }
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
