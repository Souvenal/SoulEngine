module;

#include <vk_mem_alloc.h>

export module Vulkan:Types;

export import std;
export import vulkan;

import RHI;

namespace SoulEngine {

// ═════════════════════════════════════════════════════════════════════════════
// Conversion helpers
// ═════════════════════════════════════════════════════════════════════════════

[[nodiscard]] auto ToVkFormat(RHIFormat Fmt) -> vk::Format {
    switch (Fmt) {
    case RHIFormat::R8_UNORM:
        return vk::Format::eR8Unorm;
    case RHIFormat::R8G8B8A8_UNORM:
        return vk::Format::eR8G8B8A8Unorm;
    case RHIFormat::B8G8R8A8_UNORM:
        return vk::Format::eB8G8R8A8Unorm;
    case RHIFormat::D32_SFLOAT:
        return vk::Format::eD32Sfloat;
    case RHIFormat::D24_UNORM_S8_UINT:
        return vk::Format::eD24UnormS8Uint;
    case RHIFormat::D32_SFLOAT_S8_UINT:
        return vk::Format::eD32SfloatS8Uint;
    case RHIFormat::R16G16B16A16_SFLOAT:
        return vk::Format::eR16G16B16A16Sfloat;
    case RHIFormat::R32G32B32A32_SFLOAT:
        return vk::Format::eR32G32B32A32Sfloat;
    case RHIFormat::R32G32B32_SFLOAT:
        return vk::Format::eR32G32B32Sfloat;
    case RHIFormat::R32G32_SFLOAT:
        return vk::Format::eR32G32Sfloat;
    case RHIFormat::R32_SFLOAT:
        return vk::Format::eR32Sfloat;
    case RHIFormat::R32_UINT:
        return vk::Format::eR32Uint;
    default:
        return vk::Format::eUndefined;
    }
}

[[nodiscard]] auto ToVkImageAspect(RHIFormat Fmt) -> vk::ImageAspectFlags {
    switch (Fmt) {
    case RHIFormat::D32_SFLOAT:
        return vk::ImageAspectFlagBits::eDepth;
    case RHIFormat::D32_SFLOAT_S8_UINT:
    case RHIFormat::D24_UNORM_S8_UINT:
        return vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
    default:
        return vk::ImageAspectFlagBits::eColor;
    }
}

} // namespace SoulEngine
