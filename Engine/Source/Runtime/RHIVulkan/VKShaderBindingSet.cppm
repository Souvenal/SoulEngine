module;

export module RHIVulkan:ShaderBindingSet;

import RHI;
import Shader;
import vulkan;
import std;

import :AccelerationStructure;
import :Buffer;
import :Capability;
import :Context;
import :Debug;
import :Descriptor;
import :Sampler;
import :Texture;
import Core;

namespace SoulEngine {

[[nodiscard]] auto ToVkShaderStageFlags(ShaderStage Stages) -> vk::ShaderStageFlags;

[[nodiscard]] auto GetMaxRuntimeSampledTextureCount() -> Uint32;

/// @brief Device-side ceiling for one array binding of the given resource type.
///
/// Min of per-stage and per-set limits, capped at RHI.Vulkan.MaxTextures. Used both to size unbounded layouts
/// and as the guardrail for fixed arrays.
[[nodiscard]] auto GetMaxDescriptorArrayCount(ShaderResourceType Type, ShaderBindingAccess Access) -> Uint32;

// Slang's default SPIR-V lowering assigns read-only textures to binding 2 inside the reflected bindless
// descriptor space. The descriptor set index is not hard-coded here; it comes from
// ShaderReflection::BindlessSpace. The descriptor array's slot 0 is reserved for the zero/null material
// handle; RHIRefArray supplies descriptor-facing slots starting at 1.

[[nodiscard]] auto ValidateDescriptorSetIndices(const ShaderReflection& Reflection)
    -> std::expected<void, ErrorMessage>;

[[nodiscard]] auto ValidateBindlessSpace(std::span<const RHIShaderBindingSet::Slot> Bindings)
    -> std::expected<void, ErrorMessage>;

/// @brief Vulkan descriptor layouts and sets for one reflected shader binding set.
export class VulkanShaderBindingSet final : public RHIShaderBindingSet {
  public:
    VulkanShaderBindingSet(const VulkanShaderBindingSet&)                    = delete;
    auto operator=(const VulkanShaderBindingSet&) -> VulkanShaderBindingSet& = delete;
    VulkanShaderBindingSet(VulkanShaderBindingSet&&)                         = delete;
    auto operator=(VulkanShaderBindingSet&&) -> VulkanShaderBindingSet&      = delete;

    [[nodiscard]] static auto Create(const VulkanResourceContext&   Context,
                                     VulkanDescriptorManager&       Descriptors,
                                     StringView                     Name,
                                     const RHIShaderBindingSetDesc& Desc)
        -> std::expected<UPtr<VulkanShaderBindingSet>, ErrorMessage>;

    [[nodiscard]] auto GetDescriptorSetLayouts() const -> std::span<const vk::DescriptorSetLayout>;

    [[nodiscard]] auto GetShaderStageFlags() const -> vk::ShaderStageFlags;

    [[nodiscard]] auto GetPushConstantRanges() const -> const std::vector<vk::PushConstantRange>&;

    [[nodiscard]] auto GetPipelineLayout() const -> vk::PipelineLayout;

    /// @brief The current resource combination resolved for every set through the device-wide descriptor
    /// cache.
    ///
    /// Cache hits also refresh the entry LRU.
    struct PipelineBindingState {
        std::vector<vk::DescriptorSet> DescriptorSets = {};
        std::vector<Uint32>            DynamicOffsets = {};
        std::vector<PushConstantWrite> PushConstants  = {};
    };

    [[nodiscard]] auto CaptureBindingState() -> std::expected<PipelineBindingState, ErrorMessage>;

    VulkanShaderBindingSet(String Name, const RHIShaderBindingSetDesc& Desc, VulkanDescriptorManager& Descriptors);

  protected:
    /// @brief Update the current set snapshot for the pass being recorded.
    ///
    /// RHI validation has already resolved and updated every slot in this set. Execute() calls Pass::Record()
    /// immediately before constructing its command visitor, so replacing this snapshot is intentional:
    /// CaptureBindingState() observes the bindings of the current pass and refreshes their cache LRU entries.
    [[nodiscard]] auto OnResourcesBound(Uint32 SetIndex) -> std::expected<void, ErrorMessage> override;

  private:
    VulkanDescriptorManager&                   m_Descriptors;
    std::vector<vk::DescriptorSetLayout>       m_SetLayouts         = {};
    std::vector<vk::raii::DescriptorSetLayout> m_OwnedSetLayouts    = {};
    std::vector<DescriptorSetCacheKey>         m_SetKeys            = {};
    std::vector<vk::PushConstantRange>         m_PushConstantRanges = {};
    vk::raii::PipelineLayout                   m_PipelineLayout     = nullptr;
};

} // namespace SoulEngine
