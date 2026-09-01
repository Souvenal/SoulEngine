module;

export module Vulkan:Sampler;

import Core;
import RHI;
import vulkan;
import std;

import :Capability;
import :Context;
import :Debug;

namespace SoulEngine {
namespace {

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

} // namespace

class VulkanSampler final : public RHISampler {
  public:
    VulkanSampler(String Name, const RHISamplerDesc& Desc, vk::raii::Sampler&& VulkanSampler)
        : RHISampler(std::move(Name), Desc),
          m_Sampler(std::make_shared<vk::raii::Sampler>(std::move(VulkanSampler))),
          m_DescriptorInfo{.sampler = **m_Sampler} {}

    ~VulkanSampler() override = default;

    VulkanSampler(const VulkanSampler&)                    = delete;
    auto operator=(const VulkanSampler&) -> VulkanSampler& = delete;
    VulkanSampler(VulkanSampler&&)                         = delete;
    auto operator=(VulkanSampler&&) -> VulkanSampler&      = delete;

    [[nodiscard]] static auto Create(const VulkanResourceContext& Context, StringView Name, const RHISamplerDesc& Desc)
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

    [[nodiscard]] auto GetVkSampler() const -> vk::Sampler {
        return **m_Sampler;
    }

    /// Build a sampler descriptor write for this sampler.
    [[nodiscard]] auto GetWriteDescriptorSet(vk::DescriptorSet Set,
                                             Uint32            BindingIndex,
                                             bool              /*IsReadOnly*/) const
        -> vk::WriteDescriptorSet {
        return vk::WriteDescriptorSet{
            .dstSet          = Set,
            .dstBinding      = BindingIndex,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType  = vk::DescriptorType::eSampler,
            .pImageInfo      = &m_DescriptorInfo,
        };
    }

  private:
    SPtr<vk::raii::Sampler>         m_Sampler        = nullptr;
    vk::DescriptorImageInfo m_DescriptorInfo = {};
};

} // namespace SoulEngine
