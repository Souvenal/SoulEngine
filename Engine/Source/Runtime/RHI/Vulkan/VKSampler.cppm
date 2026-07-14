module;

export module Vulkan:Sampler;

import Core;
import RHI;
import vulkan;
import std;

import :Capability;
import :DeletionQueue;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {
namespace {

struct SamplerProfileInfo {
    bool bEnableAnisotropy = false;
};

[[nodiscard]] auto GetSamplerProfileInfo(RHI::SamplerProfile Profile)
    -> std::expected<SamplerProfileInfo, ErrorMessage> {
    switch (Profile) {
    case RHI::SamplerProfile::LinearRepeat:
        return SamplerProfileInfo{};
    case RHI::SamplerProfile::AnisotropicRepeat:
        return SamplerProfileInfo{.bEnableAnisotropy = true};
    case RHI::SamplerProfile::Unknown:
        break;
    }
    return std::unexpected(ErrorMessage("Unsupported sampler profile"));
}

} // namespace

class Sampler final : public RHI::Sampler {
  public:
    Sampler(const RHI::SamplerDesc& Desc, vk::raii::Sampler&& Sampler, DeletionQueue& Queue)
        : RHI::Sampler(Desc) {
        m_Sampler       = std::make_shared<vk::raii::Sampler>(std::move(Sampler));
        m_DeletionQueue = &Queue;
    }

    ~Sampler() override {
        if (m_DeletionQueue)
            m_DeletionQueue->Enqueue(GetLastUsageToken(), [Sampler = m_Sampler]() {});
    }

    Sampler(const Sampler&)                    = delete;
    auto operator=(const Sampler&) -> Sampler& = delete;
    Sampler(Sampler&&)                         = delete;
    auto operator=(Sampler&&) -> Sampler&      = delete;

    [[nodiscard]] static auto Create(const RHI::SamplerDesc& Desc, vk::raii::Device& Device, DeletionQueue& Queue)
        -> std::expected<UPtr<RHI::Sampler>, ErrorMessage> {
        auto ProfileInfo = GetSamplerProfileInfo(Desc.Profile);
        if (!ProfileInfo)
            return std::unexpected(ProfileInfo.error().Append("Sampler::Create: invalid sampler profile"));
        if (ProfileInfo->bEnableAnisotropy && !Capability::Get().GetFeatures().samplerAnisotropy)
            return std::unexpected(ErrorMessage("Sampler::Create: sampler anisotropy feature is not supported"));

        const auto& Limits        = Capability::Get().GetProperties().limits;
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
        auto Result = Device.createSampler(SamplerCI);
        if (Result.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Sampler::Create: failed to create VkSampler"));

        return std::make_unique<Sampler>(Desc, std::move(Result.value), Queue);
    }

    [[nodiscard]] auto GetVkSampler() const -> vk::Sampler {
        return **m_Sampler;
    }

  private:
    SPtr<vk::raii::Sampler> m_Sampler       = nullptr;
    DeletionQueue*          m_DeletionQueue = nullptr;
};

} // namespace SoulEngine::RHI::Vulkan
