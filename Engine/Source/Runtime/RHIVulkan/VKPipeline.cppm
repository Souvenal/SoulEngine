module;

#include <cstddef>

export module RHIVulkan.Pipeline;

import vulkan;

import RHI;
import Shader;
import std;

import RHIVulkan.Types;
import RHIVulkan.Shader;
import RHIVulkan.ShaderBindingSet;
import RHIVulkan.Context;
import RHIVulkan.Debug;
import Core;

namespace SoulEngine {

export [[nodiscard]] auto CountDynamicOffsets(const ShaderReflection& Reflection) -> Uint32;

export struct VulkanReflectedDescriptorBinding {
    String             ParameterPath      = {};
    Uint32             Set                = 0;
    Uint32             Binding            = 0;
    ShaderResourceType Type               = ShaderResourceType::Unknown;
    Uint32             ArrayCount         = 1;
    Uint32             DynamicOffsetIndex = std::numeric_limits<Uint32>::max();
};

export [[nodiscard]] auto BuildReflectedBindings(const ShaderReflection& Reflection)
    -> std::vector<VulkanReflectedDescriptorBinding>;

// VulkanGraphicsPipeline — concrete Vulkan pipeline kept alive by RHIRef owners.

/// @brief Vulkan graphics pipeline using the layout owned by its shader binding set.
///
/// Created via the static `Create` factory. RHICommand lists only observe it.
export class VulkanGraphicsPipeline final : public RHIGraphicsPipeline {
  public:
    explicit VulkanGraphicsPipeline(String Name, const RHIGraphicsPipelineDesc& Desc)
        : RHIGraphicsPipeline(std::move(Name), Desc.BindingSet) {}

    ~VulkanGraphicsPipeline() override = default;

    VulkanGraphicsPipeline(const VulkanGraphicsPipeline&)                    = delete;
    auto operator=(const VulkanGraphicsPipeline&) -> VulkanGraphicsPipeline& = delete;

    /// @brief Create a Vulkan graphics pipeline from an RHI descriptor.
    ///
    /// Shader modules are transient — destroyed when this function returns.
    [[nodiscard]] static auto
    Create(const VulkanResourceContext& Context, StringView Name, const RHIGraphicsPipelineDesc& Desc)
        -> std::expected<UPtr<VulkanGraphicsPipeline>, ErrorMessage>;

    /// Return the native VkPipeline handle.
    [[nodiscard]] auto Get() const -> vk::Pipeline {
        return *(*m_VkPipeline);
    }

    [[nodiscard]] auto GetBindings() const -> std::span<const VulkanReflectedDescriptorBinding> {
        return m_Bindings;
    }

    [[nodiscard]] auto GetDynamicOffsetCount() const -> Uint32 {
        return m_DynamicOffsetCount;
    }

    [[nodiscard]] auto IsCompatibleWith(std::span<const RHIFormat> ColorFormats, RHIFormat DepthFormat) const -> bool {
        return std::ranges::equal(m_ColorFormats, ColorFormats) && m_DepthFormat == DepthFormat;
    }

  private:
    SPtr<vk::raii::Pipeline>                      m_VkPipeline         = nullptr;
    std::vector<VulkanReflectedDescriptorBinding> m_Bindings           = {};
    Uint32                                        m_DynamicOffsetCount = 0;
    std::vector<RHIFormat>                        m_ColorFormats       = {};
    RHIFormat                                     m_DepthFormat        = RHIFormat::Unknown;
};

// VulkanComputePipeline — concrete Vulkan compute pipeline.

export class VulkanComputePipeline final : public RHIComputePipeline {
  public:
    explicit VulkanComputePipeline(String Name, const RHIComputePipelineDesc& Desc)
        : RHIComputePipeline(std::move(Name), Desc.BindingSet) {}

    ~VulkanComputePipeline() override = default;

    VulkanComputePipeline(const VulkanComputePipeline&)                    = delete;
    auto operator=(const VulkanComputePipeline&) -> VulkanComputePipeline& = delete;

    [[nodiscard]] static auto
    Create(const VulkanResourceContext& Context, StringView Name, const RHIComputePipelineDesc& Desc)
        -> std::expected<UPtr<VulkanComputePipeline>, ErrorMessage>;

    [[nodiscard]] auto Get() const -> vk::Pipeline {
        return *(*m_VkPipeline);
    }

  private:
    SPtr<vk::raii::Pipeline> m_VkPipeline = nullptr;
};

} // namespace SoulEngine
