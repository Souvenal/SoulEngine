module;

// magic_enum::enum_flags_test is not exported by the v0.9.8 module.
#include <magic_enum/magic_enum_flags.hpp>

module RHIVulkan:Types.impl;

import :Types;

import vulkan;

import RHI;

import :Capability;

namespace SoulEngine {

auto ToVkFormat(RHIFormat Fmt) -> vk::Format {
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

auto ToVkBufferAccess(RHIBufferUsage Usage, RHIAccess Access) -> vk::AccessFlags2 {
    const bool R   = magic_enum::enum_flags_test(Access, RHIAccess::Read);
    const bool W   = magic_enum::enum_flags_test(Access, RHIAccess::Write);
    auto       Out = vk::AccessFlags2{};
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::Vertex))
        Out |= vk::AccessFlagBits2::eVertexAttributeRead;
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::Index))
        Out |= vk::AccessFlagBits2::eIndexRead;
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::Constant))
        Out |= vk::AccessFlagBits2::eUniformRead;
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::Storage))
        Out |= (R ? vk::AccessFlagBits2::eShaderStorageRead : vk::AccessFlags2{}) |
               (W ? vk::AccessFlagBits2::eShaderStorageWrite : vk::AccessFlags2{});
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::Indirect))
        Out |= vk::AccessFlagBits2::eIndirectCommandRead;
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::TransferSrc))
        Out |= vk::AccessFlagBits2::eTransferRead;
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::TransferDst))
        Out |= vk::AccessFlagBits2::eTransferWrite;
    // Geometry inputs feed acceleration-structure builds (ToVkBufferUsage adds the matching usage bit
    // unconditionally when ray tracing is available).
    if ((magic_enum::enum_flags_test(Usage, RHIBufferUsage::Vertex) ||
         magic_enum::enum_flags_test(Usage, RHIBufferUsage::Index)) &&
        VulkanCapability::Get().IsRayTracingAvailable())
        Out |= vk::AccessFlagBits2::eAccelerationStructureReadKHR;
    return Out;
}

auto ToVkTextureAccess(RHITextureUsage Usage, RHIAccess Access) -> vk::AccessFlags2 {
    const bool R   = magic_enum::enum_flags_test(Access, RHIAccess::Read);
    const bool W   = magic_enum::enum_flags_test(Access, RHIAccess::Write);
    auto       Out = vk::AccessFlags2{};
    if (magic_enum::enum_flags_test(Usage, RHITextureUsage::RenderTarget))
        Out |= (R ? vk::AccessFlagBits2::eColorAttachmentRead : vk::AccessFlags2{}) |
               (W ? vk::AccessFlagBits2::eColorAttachmentWrite : vk::AccessFlags2{});
    if (magic_enum::enum_flags_test(Usage, RHITextureUsage::DepthStencil))
        Out |= vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    if (magic_enum::enum_flags_test(Usage, RHITextureUsage::Storage))
        Out |= (R ? vk::AccessFlagBits2::eShaderStorageRead : vk::AccessFlags2{}) |
               (W ? vk::AccessFlagBits2::eShaderStorageWrite : vk::AccessFlags2{});
    if (magic_enum::enum_flags_test(Usage, RHITextureUsage::Sampled))
        Out |= vk::AccessFlagBits2::eShaderSampledRead;
    if (magic_enum::enum_flags_test(Usage, RHITextureUsage::TransferSrc))
        Out |= vk::AccessFlagBits2::eTransferRead;
    if (magic_enum::enum_flags_test(Usage, RHITextureUsage::TransferDst))
        Out |= vk::AccessFlagBits2::eTransferWrite;
    return Out;
}

auto ToVkImageLayout(RHITextureUsage Usage, RHIAccess Access) -> vk::ImageLayout {
    const auto Bits = static_cast<Uint32>(Usage);
    if (Bits == 0)
        return vk::ImageLayout::eUndefined;
    if ((Bits & (Bits - 1)) != 0 || magic_enum::enum_flags_test(Usage, RHITextureUsage::Storage))
        return vk::ImageLayout::eGeneral;
    switch (Usage) {
    case RHITextureUsage::RenderTarget:
        return vk::ImageLayout::eColorAttachmentOptimal;
    case RHITextureUsage::DepthStencil:
        return magic_enum::enum_flags_test(Access, RHIAccess::Write) ? vk::ImageLayout::eDepthAttachmentOptimal
                                                                     : vk::ImageLayout::eDepthStencilReadOnlyOptimal;
    case RHITextureUsage::Sampled:
        return vk::ImageLayout::eShaderReadOnlyOptimal;
    case RHITextureUsage::TransferSrc:
        return vk::ImageLayout::eTransferSrcOptimal;
    case RHITextureUsage::TransferDst:
        return vk::ImageLayout::eTransferDstOptimal;
    default:
        return vk::ImageLayout::eUndefined;
    }
}

auto ToVkImageAspect(RHIFormat Fmt) -> vk::ImageAspectFlags {
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

auto ToVkPipelineStages(RHISyncStage Stage) -> vk::PipelineStageFlags2 {
    vk::PipelineStageFlags2 Result{};
    if (magic_enum::enum_flags_test(Stage, RHISyncStage::VertexShader))
        Result |= vk::PipelineStageFlagBits2::eVertexShader;
    if (magic_enum::enum_flags_test(Stage, RHISyncStage::FragmentShader))
        Result |= vk::PipelineStageFlagBits2::eFragmentShader;
    if (magic_enum::enum_flags_test(Stage, RHISyncStage::ComputeShader))
        Result |= vk::PipelineStageFlagBits2::eComputeShader;
    if (magic_enum::enum_flags_test(Stage, RHISyncStage::RayTracingShader))
        Result |= vk::PipelineStageFlagBits2::eRayTracingShaderKHR;
    if (magic_enum::enum_flags_test(Stage, RHISyncStage::DrawIndirect))
        Result |= vk::PipelineStageFlagBits2::eDrawIndirect;
    if (magic_enum::enum_flags_test(Stage, RHISyncStage::ColorAttachmentOutput))
        Result |= vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    if (magic_enum::enum_flags_test(Stage, RHISyncStage::EarlyFragmentTests))
        Result |= vk::PipelineStageFlagBits2::eEarlyFragmentTests;
    if (magic_enum::enum_flags_test(Stage, RHISyncStage::LateFragmentTests))
        Result |= vk::PipelineStageFlagBits2::eLateFragmentTests;
    if (magic_enum::enum_flags_test(Stage, RHISyncStage::Transfer))
        Result |= vk::PipelineStageFlagBits2::eTransfer;
    if (magic_enum::enum_flags_test(Stage, RHISyncStage::Host))
        Result |= vk::PipelineStageFlagBits2::eHost;
    return Result;
}

} // namespace SoulEngine
