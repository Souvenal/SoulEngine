module RHIVulkan.Sampler;

import Core;
import RHI;
import vulkan;
import std;

import RHIVulkan.Capability;
import RHIVulkan.Context;
import RHIVulkan.Debug;

namespace SoulEngine {

struct VulkanSamplerProfileInfo {
    bool bEnableAnisotropy = false;
};

[[nodiscard]] auto GetSamplerProfileInfo(RHISamplerProfile Profile)
    -> std::expected<VulkanSamplerProfileInfo, ErrorMessage> {
    switch (Profile) {
    case RHISamplerProfile::LinearRepeat:
        return VulkanSamplerProfileInfo{};
    case RHISamplerProfile::AnisotropicRepeat:
        return VulkanSamplerProfileInfo{.bEnableAnisotropy = true};
    case RHISamplerProfile::Unknown:
        break;
    }
    return std::unexpected(ErrorMessage("Unsupported sampler profile"));
}

VulkanSampler::VulkanSampler(String Name, const RHISamplerDesc& Desc, vk::raii::Sampler&& VulkanSampler)
    : RHISampler(std::move(Name), Desc), m_Sampler(std::move(VulkanSampler)) {}

auto VulkanSampler::Create(const VulkanResourceContext& Context, StringView Name, const RHISamplerDesc& Desc)
    -> std::expected<UPtr<RHISampler>, ErrorMessage> {
    auto ProfileInfo = GetSamplerProfileInfo(Desc.Profile);
    if (!ProfileInfo)
        return std::unexpected(ProfileInfo.error().Append("VulkanSampler::Create: invalid sampler profile"));
    if (ProfileInfo->bEnableAnisotropy && !VulkanCapability::Get().GetFeatures().samplerAnisotropy)
        return std::unexpected(ErrorMessage("VulkanSampler::Create: sampler anisotropy feature is not supported"));

    const auto& Limits        = VulkanCapability::Get().GetProperties().limits;
    const auto  MaxAnisotropy = ProfileInfo->bEnableAnisotropy ? Limits.maxSamplerAnisotropy : 1.0f;

    vk::SamplerCreateInfo SamplerCI{
        .magFilter               = vk::Filter::eLinear,
        .minFilter               = vk::Filter::eLinear,
        .mipmapMode              = vk::SamplerMipmapMode::eLinear,
        .addressModeU            = vk::SamplerAddressMode::eRepeat,
        .addressModeV            = vk::SamplerAddressMode::eRepeat,
        .addressModeW            = vk::SamplerAddressMode::eRepeat,
        .mipLodBias              = 0.0f,
        .anisotropyEnable        = ProfileInfo->bEnableAnisotropy,
        .maxAnisotropy           = MaxAnisotropy,
        .compareEnable           = vk::False,
        .compareOp               = vk::CompareOp::eAlways,
        .minLod                  = 0.0f,
        .maxLod                  = vk::LodClampNone,
        .borderColor             = vk::BorderColor::eIntOpaqueBlack,
        .unnormalizedCoordinates = vk::False,
    };
    auto Result = Context.GetDevice().createSampler(SamplerCI);
    if (Result.result != vk::Result::eSuccess)
        return std::unexpected(ErrorMessage("VulkanSampler::Create: failed to create VkSampler"));

    Context.GetDebugUtils().SetObjectName(*Result.value, Name);
    return std::make_unique<VulkanSampler>(String(Name), Desc, std::move(Result.value));
}

auto VulkanSampler::GetVkSampler() const -> vk::Sampler {
    return *m_Sampler;
}

} // namespace SoulEngine
