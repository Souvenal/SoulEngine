/// Vulkan shader reflection helpers and shader-module management.
///
/// Converts RHI shader resource and vertex-input types to their Vulkan
/// equivalents, and provides CreateShaderStages for turning a
/// GraphicsPipelineDesc into live VkShaderModule + PipelineShaderStageCreateInfo
/// arrays.
///
/// Internal to the Vulkan RHI backend; imported by VKPipeline and (in future)
/// by compute/ray-tracing pipeline creation.

module;

export module Vulkan:Shader;

import vulkan;

import Core;
import Shader;
import RHI;
import std;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

// ═════════════════════════════════════════════════════════════════════════════
// Resource-type conversion
// ═════════════════════════════════════════════════════════════════════════════

[[nodiscard]] auto ToVkDescriptorType(Shader::ResourceType ResourceType)
    -> std::expected<vk::DescriptorType, ErrorMessage> {
    switch (ResourceType) {
    case Shader::ResourceType::Unknown:
        return std::unexpected(ErrorMessage("Cannot lower Unknown shader resource type to Vulkan descriptor type"));
    case Shader::ResourceType::ConstantBuffer:
        return vk::DescriptorType::eUniformBuffer;
    case Shader::ResourceType::StorageBuffer:
        return vk::DescriptorType::eStorageBuffer;
    case Shader::ResourceType::SampledTexture:
        return vk::DescriptorType::eSampledImage;
    case Shader::ResourceType::StorageTexture:
        return vk::DescriptorType::eStorageImage;
    case Shader::ResourceType::Sampler:
        return vk::DescriptorType::eSampler;
    }
    return std::unexpected(ErrorMessage("Unsupported shader resource type in Vulkan lowering"));
}

[[nodiscard]] auto ToVkVertexFormat(const Shader::ValueType& ValueType) -> std::expected<vk::Format, ErrorMessage> {
    if (ValueType.RowCount != 1) {
        return std::unexpected(
            ErrorMessage("Matrix vertex inputs are not supported by vertex input layout validation"));
    }

    switch (ValueType.ScalarType) {
    case Shader::ScalarType::Float32:
        switch (ValueType.ColumnCount) {
        case 1:
            return vk::Format::eR32Sfloat;
        case 2:
            return vk::Format::eR32G32Sfloat;
        case 3:
            return vk::Format::eR32G32B32Sfloat;
        case 4:
            return vk::Format::eR32G32B32A32Sfloat;
        default:
            break;
        }
        break;
    case Shader::ScalarType::Int32:
        switch (ValueType.ColumnCount) {
        case 1:
            return vk::Format::eR32Sint;
        case 2:
            return vk::Format::eR32G32Sint;
        case 3:
            return vk::Format::eR32G32B32Sint;
        case 4:
            return vk::Format::eR32G32B32A32Sint;
        default:
            break;
        }
        break;
    case Shader::ScalarType::Uint32:
        switch (ValueType.ColumnCount) {
        case 1:
            return vk::Format::eR32Uint;
        case 2:
            return vk::Format::eR32G32Uint;
        case 3:
            return vk::Format::eR32G32B32Uint;
        case 4:
            return vk::Format::eR32G32B32A32Uint;
        default:
            break;
        }
        break;
    default:
        break;
    }

    return std::unexpected(
        ErrorMessage(Core::Format("Unsupported reflected vertex input type: scalar={}, rows={}, cols={}",
                                  static_cast<int>(ValueType.ScalarType),
                                  ValueType.RowCount,
                                  ValueType.ColumnCount)));
}

[[nodiscard]] auto ToVkVertexFormat(RHI::Format Format) -> std::expected<vk::Format, ErrorMessage> {
    switch (Format) {
    case RHI::Format::R32_SFLOAT:
        return vk::Format::eR32Sfloat;
    case RHI::Format::R32G32_SFLOAT:
        return vk::Format::eR32G32Sfloat;
    case RHI::Format::R32G32B32_SFLOAT:
        return vk::Format::eR32G32B32Sfloat;
    case RHI::Format::R32G32B32A32_SFLOAT:
        return vk::Format::eR32G32B32A32Sfloat;
    default:
        return std::unexpected(
            ErrorMessage(Core::Format("Unsupported explicit vertex input format: {}", static_cast<int>(Format))));
    }
}

[[nodiscard]] auto FindReflectedVertexInputByLocation(const std::vector<Shader::VertexInputAttribute>& VertexInputs,
                                                      Uint32                                           Location)
    -> const Shader::VertexInputAttribute* {
    for (const auto& Attr : VertexInputs) {
        if (Attr.Location && *Attr.Location == Location)
            return &Attr;
    }
    return nullptr;
}

[[nodiscard]] auto ValidateVertexInputLayout(const GraphicsPipelineDesc& Desc) -> std::expected<void, ErrorMessage> {
    const auto& ExplicitLayout = Desc.VertexInputLayout;
    std::array<bool, kMaxVertexBufferBindings> DeclaredBindings = {};
    for (const auto& Binding : ExplicitLayout.Bindings) {
        if (Binding.Binding >= kMaxVertexBufferBindings) {
            return std::unexpected(
                ErrorMessage(Core::Format("Vertex input binding {} exceeds supported binding count {}", Binding.Binding,
                                          kMaxVertexBufferBindings)));
        }
        if (Binding.Stride == 0)
            return std::unexpected(
                ErrorMessage(Core::Format("Vertex input binding {} has zero stride", Binding.Binding)));
        if (DeclaredBindings[Binding.Binding])
            return std::unexpected(
                ErrorMessage(Core::Format("Vertex input binding {} is duplicated", Binding.Binding)));
        DeclaredBindings[Binding.Binding] = true;
    }

    for (const auto& Attr : ExplicitLayout.Attributes) {
        if (Attr.Binding >= kMaxVertexBufferBindings || !DeclaredBindings[Attr.Binding]) {
            return std::unexpected(
                ErrorMessage(Core::Format("Vertex input attribute location {} references undeclared binding {}",
                                          Attr.Location,
                                          Attr.Binding)));
        }
    }

    const auto& ReflectedInputs = Desc.Program.Reflection.VertexInputs;
    if (ReflectedInputs.empty())
        return {};

    if (ExplicitLayout.Attributes.empty()) {
        LogWarning("Vertex shader '{}' reflects {} vertex input(s), but pipeline has no explicit vertex input layout",
                   Desc.Program.VertexEntryPointName,
                   ReflectedInputs.size());
        return {};
    }

    for (const auto& Attr : ReflectedInputs) {
        if (!Attr.Location) {
            LogWarning("Vertex shader '{}' input '{}{}' has no reflected location; explicit layout validation skipped",
                       Desc.Program.VertexEntryPointName,
                       Attr.SemanticName,
                       Attr.SemanticIndex);
            continue;
        }

        auto It = std::find_if(ExplicitLayout.Attributes.begin(),
                               ExplicitLayout.Attributes.end(),
                               [&](const VertexInputAttributeDesc& DescAttr) -> bool {
                                   return DescAttr.Location == *Attr.Location;
                               });
        if (It == ExplicitLayout.Attributes.end()) {
            LogWarning("Vertex shader '{}' expects input at location {}, but explicit vertex layout does not provide it",
                       Desc.Program.VertexEntryPointName,
                       *Attr.Location);
            continue;
        }

        auto ExpectedFormat = ToVkVertexFormat(Attr.ValueType);
        auto ActualFormat   = ToVkVertexFormat(It->Format);
        if (!ExpectedFormat || !ActualFormat) {
            LogWarning("Vertex shader '{}' location {} format validation skipped: reflected format ok={}, explicit "
                       "format ok={}",
                       Desc.Program.VertexEntryPointName,
                       *Attr.Location,
                       ExpectedFormat.has_value(),
                       ActualFormat.has_value());
            continue;
        }

        if (*ExpectedFormat != *ActualFormat) {
            LogWarning("Vertex shader '{}' location {} expects format {}, but explicit vertex layout provides {}",
                       Desc.Program.VertexEntryPointName,
                       *Attr.Location,
                       vk::to_string(*ExpectedFormat),
                       vk::to_string(*ActualFormat));
        }
    }

    for (const auto& Attr : ExplicitLayout.Attributes) {
        if (FindReflectedVertexInputByLocation(ReflectedInputs, Attr.Location) == nullptr) {
            LogWarning("Explicit vertex layout provides location {}, but vertex shader '{}' does not consume it",
                       Attr.Location,
                       Desc.Program.VertexEntryPointName);
        }
    }

    return {};
}

//
// ──────────────────────────────────────────────────────────────────────────
// Shader-module management
// ──────────────────────────────────────────────────────────────────────────

/// Collects shader modules, stage-create-infos, and vertex input state
/// for graphics pipeline creation.
///
/// Owns VkShaderModule lifetimes; all modules are destroyed when this
/// object is destroyed.
class GraphicsShaderStates {
  public:
    std::vector<vk::PipelineShaderStageCreateInfo> StageInfos = {};

    /// Create shader modules, stage infos, and vertex input state from a
    /// graphics pipeline descriptor.
    [[nodiscard]] static auto Create(const vk::raii::Device& Device, const GraphicsPipelineDesc& Desc)
        -> std::expected<GraphicsShaderStates, ErrorMessage> {
        GraphicsShaderStates Result;

        if (Desc.Program.Code.empty())
            return std::unexpected(ErrorMessage("Graphics shader program has no SPIR-V code"));

        const auto& CodeVec = Desc.Program.Code;
        vk::ShaderModuleCreateInfo ModuleCI{
            .codeSize = CodeVec.size() * sizeof(Uint32),
            .pCode    = CodeVec.data(),
        };
        auto [Res, Module] = Device.createShaderModule(ModuleCI);
        if (Res != vk::Result::eSuccess) {
            return std::unexpected(ErrorMessage(Core::Format(
                "Failed to create shader module for graphics program '{} + {}': {}",
                Desc.Program.VertexEntryPointName,
                Desc.Program.FragmentEntryPointName,
                vk::to_string(Res))));
        }

        Result.m_Modules.push_back(std::move(Module));
        Result.StageInfos.push_back(vk::PipelineShaderStageCreateInfo{
            .stage  = vk::ShaderStageFlagBits::eVertex,
            .module = *Result.m_Modules.back(),
            .pName  = Desc.Program.VertexEntryPointName.c_str(),
        });
        Result.StageInfos.push_back(vk::PipelineShaderStageCreateInfo{
            .stage  = vk::ShaderStageFlagBits::eFragment,
            .module = *Result.m_Modules.back(),
            .pName  = Desc.Program.FragmentEntryPointName.c_str(),
        });

        // ── Vertex input state ──────────────────────────────────────────────────
        // The explicit CPU vertex-buffer layout is authoritative. Shader
        // reflection is used only to warn about location/format mismatches.
        if (auto R = ValidateVertexInputLayout(Desc); !R)
            return std::unexpected(R.error());
        if (!Desc.VertexInputLayout.Attributes.empty()) {
            Result.m_VertexAttributes.reserve(Desc.VertexInputLayout.Attributes.size());

            for (const auto& Attr : Desc.VertexInputLayout.Attributes) {
                auto VkFormat = ToVkVertexFormat(Attr.Format);
                if (!VkFormat)
                    return std::unexpected(VkFormat.error().Append(Core::Format(
                        "Failed to lower explicit vertex input layout location {}", Attr.Location)));

                Result.m_VertexAttributes.push_back(vk::VertexInputAttributeDescription{
                    .location = Attr.Location,
                    .binding  = Attr.Binding,
                    .format   = *VkFormat,
                    .offset   = Attr.Offset,
                });
            }

            Result.m_VertexBindings.reserve(Desc.VertexInputLayout.Bindings.size());
            for (const auto& Binding : Desc.VertexInputLayout.Bindings) {
                Result.m_VertexBindings.push_back(vk::VertexInputBindingDescription{
                    .binding   = Binding.Binding,
                    .stride    = Binding.Stride,
                    .inputRate = vk::VertexInputRate::eVertex,
                });
            }
        }

        return Result;
    }

    /// @brief Build a vertex-input-state create-info whose pointers are valid
    /// for the lifetime of this GraphicsShaderStates object.
    ///
    /// Delay the construction of CI rather than contructing in` Create`,
    /// Because anything in `Create` is a temp value, leading to hanging pointers.
    [[nodiscard]] auto GetPipelineVertexInputStateCI() const -> vk::PipelineVertexInputStateCreateInfo {
        if (m_VertexAttributes.empty())
            return {};
        return vk::PipelineVertexInputStateCreateInfo{
            .vertexBindingDescriptionCount   = static_cast<Uint32>(m_VertexBindings.size()),
            .pVertexBindingDescriptions      = m_VertexBindings.data(),
            .vertexAttributeDescriptionCount = static_cast<Uint32>(m_VertexAttributes.size()),
            .pVertexAttributeDescriptions    = m_VertexAttributes.data(),
        };
    }

  private:
    std::vector<vk::raii::ShaderModule>              m_Modules           = {};
    std::vector<vk::VertexInputBindingDescription>   m_VertexBindings    = {};
    std::vector<vk::VertexInputAttributeDescription> m_VertexAttributes  = {};
};

} // namespace SoulEngine::RHI::Vulkan
