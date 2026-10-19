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

class VulkanSampler final : public RHISampler {
  public:
    VulkanSampler(String Name, const RHISamplerDesc& Desc, vk::raii::Sampler&& VulkanSampler);

    ~VulkanSampler() override = default;

    VulkanSampler(const VulkanSampler&)                    = delete;
    auto operator=(const VulkanSampler&) -> VulkanSampler& = delete;
    VulkanSampler(VulkanSampler&&)                         = delete;
    auto operator=(VulkanSampler&&) -> VulkanSampler&      = delete;

    [[nodiscard]] static auto Create(const VulkanResourceContext& Context, StringView Name, const RHISamplerDesc& Desc)
        -> std::expected<UPtr<RHISampler>, ErrorMessage>;

    [[nodiscard]] auto GetVkSampler() const -> vk::Sampler;

  private:
    vk::raii::Sampler m_Sampler;
};

} // namespace SoulEngine
