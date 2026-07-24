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
    case RHIFormat::R32G32B32A32_SFLOAT:
        return vk::Format::eR32G32B32A32Sfloat;
    case RHIFormat::R32G32B32_SFLOAT:
        return vk::Format::eR32G32B32Sfloat;
    case RHIFormat::R32G32_SFLOAT:
        return vk::Format::eR32G32Sfloat;
    case RHIFormat::R32_SFLOAT:
        return vk::Format::eR32Sfloat;
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

// ═════════════════════════════════════════════════════════════════════════════
// Barrier state tracking (Sync2)
// ═════════════════════════════════════════════════════════════════════════════

/// Per-buffer GPU state for automatic barrier generation.
/// Buffers have no image-layout concept; layout tracking is not needed.
struct VulkanBufferState {
    vk::PipelineStageFlags2 stage       = vk::PipelineStageFlagBits2::eNone;
    vk::AccessFlags2        access      = vk::AccessFlagBits2::eNone;
    Uint32                  queueFamily = vk::QueueFamilyIgnored;
    bool                    isWrite     = false;
};

auto VulkanTransitionBuffer(vk::raii::CommandBuffer&                Buf,
                      std::unordered_map<vk::Buffer, VulkanBufferState>& States,
                      vk::Buffer                                   Buffer,
                      vk::PipelineStageFlags2                     DstStage,
                      vk::AccessFlags2                            DstAccess,
                      bool                                         IsWrite,
                      vk::DeviceSize                               Offset = 0,
                      vk::DeviceSize                               Size   = vk::WholeSize) -> void {
    auto It      = States.find(Buffer);
    auto Current = (It != States.end()) ? It->second : VulkanBufferState{};

    const bool NeedsBarrier = (Current.stage != DstStage) || (Current.access != DstAccess) || Current.isWrite;
    if (NeedsBarrier) {
        vk::BufferMemoryBarrier2 Barrier{
            .srcStageMask        = Current.stage,
            .srcAccessMask       = Current.access,
            .dstStageMask        = DstStage,
            .dstAccessMask       = DstAccess,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .buffer              = Buffer,
            .offset              = Offset,
            .size                = Size,
        };
        vk::DependencyInfo Dep{
            .dependencyFlags          = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers    = &Barrier,
        };
        Buf.pipelineBarrier2(Dep);
    }

    States[Buffer] = VulkanBufferState{
        .stage       = DstStage,
        .access      = DstAccess,
        .queueFamily = vk::QueueFamilyIgnored,
        .isWrite     = IsWrite,
    };
}

/// Per-image GPU state for automatic barrier generation.
/// Default layout = eUndefined so first-use transitions derive the correct
/// srcLayout without special-case init logic.
struct VulkanImageState {
    vk::PipelineStageFlags2 stage       = vk::PipelineStageFlagBits2::eNone;
    vk::AccessFlags2        access      = vk::AccessFlagBits2::eNone;
    vk::ImageLayout         layout      = vk::ImageLayout::eUndefined;
    Uint32                  queueFamily = vk::QueueFamilyIgnored;
    bool                    isWrite     = false;
};

auto VulkanTransitionImage(vk::raii::CommandBuffer&              Buf,
                     std::unordered_map<vk::Image, VulkanImageState>& States,
                     vk::Image                             Image,
                     vk::PipelineStageFlags2               DstStage,
                     vk::AccessFlags2                      DstAccess,
                     vk::ImageLayout                       DstLayout,
                     bool                                  IsWrite,
                     vk::ImageAspectFlags                  Aspect = vk::ImageAspectFlagBits::eColor) -> void {
    auto It      = States.find(Image);
    auto Current = (It != States.end()) ? It->second : VulkanImageState{};

    const bool NeedsBarrier =
        (Current.stage != DstStage) || (Current.access != DstAccess) || (Current.layout != DstLayout) ||
        Current.isWrite;

    if (NeedsBarrier) {
        vk::ImageMemoryBarrier2 Barrier{
            .srcStageMask        = Current.stage,
            .srcAccessMask       = Current.access,
            .dstStageMask        = DstStage,
            .dstAccessMask       = DstAccess,
            .oldLayout           = Current.layout,
            .newLayout           = DstLayout,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image               = Image,
            .subresourceRange    = {.aspectMask     = Aspect,
                                    .baseMipLevel   = 0,
                                    .levelCount     = vk::RemainingMipLevels,
                                    .baseArrayLayer = 0,
                                    .layerCount     = vk::RemainingArrayLayers},
        };
        vk::DependencyInfo Dep{
            .dependencyFlags         = vk::DependencyFlagBits::eByRegion,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers    = &Barrier,
        };
        Buf.pipelineBarrier2(Dep);
    }

    States[Image] = VulkanImageState{
        .stage       = DstStage,
        .access      = DstAccess,
        .layout      = DstLayout,
        .queueFamily = vk::QueueFamilyIgnored,
        .isWrite     = IsWrite,
    };
}

} // namespace SoulEngine
