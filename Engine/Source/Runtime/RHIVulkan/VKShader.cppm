/// @brief Vulkan shader reflection helpers and shader-module management.
///
/// Converts RHI shader resource and vertex-input types to their Vulkan equivalents, and provides
/// CreateShaderStages for turning a RHIGraphicsPipelineDesc into live VkShaderModule +
/// PipelineShaderStageCreateInfo arrays.
///
/// Internal to the Vulkan RHI backend; imported by VKPipeline and (in future) by compute/ray-tracing pipeline
/// creation.

module;

export module RHIVulkan:Shader;

import magic_enum;
import vulkan;

import Core;
import Shader;
import RHI;
import std;
import :Debug;

namespace SoulEngine {

// Resource-type conversion

[[nodiscard]] auto ToVkVertexFormat(const ShaderValueType& ValueType) -> std::expected<vk::Format, ErrorMessage>;

[[nodiscard]] auto ToVkVertexFormat(RHIFormat Fmt) -> std::expected<vk::Format, ErrorMessage>;

[[nodiscard]] auto FindReflectedVertexInputByLocation(const std::vector<ShaderVertexInputAttribute>& VertexInputs,
                                                      Uint32 Location) -> const ShaderVertexInputAttribute*;

[[nodiscard]] auto ValidateVertexInputLayout(const RHIGraphicsPipelineDesc& Desc) -> std::expected<void, ErrorMessage>;

//
// Shader-module management

/// @brief Collects shader modules, stage-create-infos, and vertex input state for graphics pipeline
/// creation.
///
/// Owns VkShaderModule lifetimes; all modules are destroyed when this object is destroyed.
class VulkanGraphicsShaderStates {
  public:
    std::vector<vk::PipelineShaderStageCreateInfo> StageInfos = {};

    /// Create shader modules, stage infos, and vertex input state from a graphics pipeline descriptor.
    [[nodiscard]] static auto
    Create(const vk::raii::Device& Device, VulkanDebugUtils& DebugUtils, const RHIGraphicsPipelineDesc& Desc)
        -> std::expected<VulkanGraphicsShaderStates, ErrorMessage>;

    /// @brief Build a vertex-input-state create-info whose pointers are valid
    /// for the lifetime of this VulkanGraphicsShaderStates object.
    ///
    /// Delay the construction of CI rather than contructing in` Create`, Because anything in `Create` is a
    /// temp value, leading to hanging pointers.
    [[nodiscard]] auto GetPipelineVertexInputStateCI() const -> vk::PipelineVertexInputStateCreateInfo;

  private:
    std::vector<vk::raii::ShaderModule>              m_Modules          = {};
    std::vector<vk::VertexInputBindingDescription>   m_VertexBindings   = {};
    std::vector<vk::VertexInputAttributeDescription> m_VertexAttributes = {};
};

} // namespace SoulEngine
