/// Vulkan shader reflection helpers and shader-module management.
///
/// Converts RHI shader types (Shader::Stage, Shader::ResourceType, …) to
/// their Vulkan equivalents, and provides CreateShaderStages for turning a
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
// Stage / resource-type conversion
// ═════════════════════════════════════════════════════════════════════════════

[[nodiscard]] auto ToVkShaderStage(Shader::Stage Stage) -> vk::ShaderStageFlagBits {
    switch (Stage) {
    case Shader::Stage::Unknown:
        return vk::ShaderStageFlagBits::eAll;
    case Shader::Stage::Vertex:
        return vk::ShaderStageFlagBits::eVertex;
    case Shader::Stage::Fragment:
        return vk::ShaderStageFlagBits::eFragment;
    case Shader::Stage::Compute:
        return vk::ShaderStageFlagBits::eCompute;
    case Shader::Stage::Hull:
        return vk::ShaderStageFlagBits::eTessellationControl;
    case Shader::Stage::Domain:
        return vk::ShaderStageFlagBits::eTessellationEvaluation;
    case Shader::Stage::Geometry:
        return vk::ShaderStageFlagBits::eGeometry;
    case Shader::Stage::Mesh:
        return vk::ShaderStageFlagBits::eMeshEXT;
    case Shader::Stage::Amplification:
        return vk::ShaderStageFlagBits::eTaskEXT;
    }
    return vk::ShaderStageFlagBits::eAll; // unreachable for valid enum values
}

[[nodiscard]] auto ToVkShaderStages(ShaderStageMask StageMask) -> vk::ShaderStageFlags {
    vk::ShaderStageFlags Flags{};
    if ((static_cast<Uint32>(StageMask) & static_cast<Uint32>(ShaderStageMask::Vertex)) != 0)
        Flags |= vk::ShaderStageFlagBits::eVertex;
    if ((static_cast<Uint32>(StageMask) & static_cast<Uint32>(ShaderStageMask::Fragment)) != 0)
        Flags |= vk::ShaderStageFlagBits::eFragment;
    if ((static_cast<Uint32>(StageMask) & static_cast<Uint32>(ShaderStageMask::Compute)) != 0)
        Flags |= vk::ShaderStageFlagBits::eCompute;
    if ((static_cast<Uint32>(StageMask) & static_cast<Uint32>(ShaderStageMask::Hull)) != 0)
        Flags |= vk::ShaderStageFlagBits::eTessellationControl;
    if ((static_cast<Uint32>(StageMask) & static_cast<Uint32>(ShaderStageMask::Domain)) != 0)
        Flags |= vk::ShaderStageFlagBits::eTessellationEvaluation;
    if ((static_cast<Uint32>(StageMask) & static_cast<Uint32>(ShaderStageMask::Geometry)) != 0)
        Flags |= vk::ShaderStageFlagBits::eGeometry;
    if ((static_cast<Uint32>(StageMask) & static_cast<Uint32>(ShaderStageMask::Mesh)) != 0)
        Flags |= vk::ShaderStageFlagBits::eMeshEXT;
    if ((static_cast<Uint32>(StageMask) & static_cast<Uint32>(ShaderStageMask::Amplification)) != 0)
        Flags |= vk::ShaderStageFlagBits::eTaskEXT;
    return Flags;
}

[[nodiscard]] auto ToVkDescriptorType(Shader::ResourceType ResourceType)
    -> std::expected<vk::DescriptorType, ErrorMessage> {
    switch (ResourceType) {
    case Shader::ResourceType::Unknown:
        return std::unexpected(ErrorMessage("Cannot lower Unknown shader resource type to Vulkan descriptor type"));
    case Shader::ResourceType::UniformBuffer:
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

[[nodiscard]] auto ToVkVertexFormat(Format Format) -> std::expected<vk::Format, ErrorMessage> {
    switch (Format) {
    case Format::R32_SFLOAT:
        return vk::Format::eR32Sfloat;
    case Format::R32G32_SFLOAT:
        return vk::Format::eR32G32Sfloat;
    case Format::R32G32B32_SFLOAT:
        return vk::Format::eR32G32B32Sfloat;
    case Format::R32G32B32A32_SFLOAT:
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

auto ValidateVertexInputLayout(const GraphicsPipelineDesc& Desc) -> void {
    if (!Desc.VertexProgram.Reflection)
        return;

    const auto& ReflectedInputs = Desc.VertexProgram.Reflection->VertexInputs;
    if (ReflectedInputs.empty())
        return;

    const auto& ExplicitLayout = Desc.VertexInputLayout;
    if (ExplicitLayout.Attributes.empty()) {
        LogWarning("Vertex shader '{}' reflects {} vertex input(s), but pipeline has no explicit vertex input layout",
                   Desc.VertexProgram.EntryPointName,
                   ReflectedInputs.size());
        return;
    }

    for (const auto& Attr : ReflectedInputs) {
        if (!Attr.Location) {
            LogWarning("Vertex shader '{}' input '{}{}' has no reflected location; explicit layout validation skipped",
                       Desc.VertexProgram.EntryPointName,
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
                       Desc.VertexProgram.EntryPointName,
                       *Attr.Location);
            continue;
        }

        auto ExpectedFormat = ToVkVertexFormat(Attr.ValueType);
        auto ActualFormat   = ToVkVertexFormat(It->Format);
        if (!ExpectedFormat || !ActualFormat) {
            LogWarning("Vertex shader '{}' location {} format validation skipped: reflected format ok={}, explicit "
                       "format ok={}",
                       Desc.VertexProgram.EntryPointName,
                       *Attr.Location,
                       ExpectedFormat.has_value(),
                       ActualFormat.has_value());
            continue;
        }

        if (*ExpectedFormat != *ActualFormat) {
            LogWarning("Vertex shader '{}' location {} expects format {}, but explicit vertex layout provides {}",
                       Desc.VertexProgram.EntryPointName,
                       *Attr.Location,
                       vk::to_string(*ExpectedFormat),
                       vk::to_string(*ActualFormat));
        }
    }

    for (const auto& Attr : ExplicitLayout.Attributes) {
        if (FindReflectedVertexInputByLocation(ReflectedInputs, Attr.Location) == nullptr) {
            LogWarning("Explicit vertex layout provides location {}, but vertex shader '{}' does not consume it",
                       Attr.Location,
                       Desc.VertexProgram.EntryPointName);
        }
    }
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

        auto AddShaderStage = [&](const Shader::Program& Program) -> std::expected<void, ErrorMessage> {
            const auto&                CodeVec = *Program.Code;
            vk::ShaderModuleCreateInfo ModuleCI{
                .codeSize = CodeVec.size() * sizeof(Uint32),
                .pCode    = CodeVec.data(),
            };
            auto [Res, Module] = Device.createShaderModule(ModuleCI);
            if (Res != vk::Result::eSuccess) {
                return std::unexpected(ErrorMessage(Core::Format(
                    "Failed to create shader module for '{}': {}", Program.EntryPointName, vk::to_string(Res))));
            }

            Result.m_Modules.push_back(std::move(Module));
            auto StageCI = vk::PipelineShaderStageCreateInfo{
                .stage  = ToVkShaderStage(Program.Stage),
                .module = *Result.m_Modules.back(),
                .pName  = Program.EntryPointName.c_str(),
            };
            Result.StageInfos.push_back(StageCI);
            return {};
        };

        if (auto R = AddShaderStage(Desc.VertexProgram); !R)
            return std::unexpected(R.error());

        if (Desc.FragmentProgram) {
            if (auto R = AddShaderStage(*Desc.FragmentProgram); !R)
                return std::unexpected(R.error());
        }

        // ── Vertex input state ──────────────────────────────────────────────────
        // The explicit CPU vertex-buffer layout is authoritative. Shader
        // reflection is used only to warn about location/format mismatches.
        // TODO: Support multiple vertex buffer bindings and per-instance input rate.
        ValidateVertexInputLayout(Desc);
        if (!Desc.VertexInputLayout.Attributes.empty()) {
            Result.m_VertexAttributes.reserve(Desc.VertexInputLayout.Attributes.size());

            for (const auto& Attr : Desc.VertexInputLayout.Attributes) {
                auto VkFormat = ToVkVertexFormat(Attr.Format);
                if (!VkFormat)
                    return std::unexpected(VkFormat.error().Append(Core::Format(
                        "Failed to lower explicit vertex input layout location {}", Attr.Location)));

                Result.m_VertexAttributes.push_back(vk::VertexInputAttributeDescription{
                    .location = Attr.Location,
                    .binding  = Desc.VertexInputLayout.Binding,
                    .format   = *VkFormat,
                    .offset   = Attr.Offset,
                });
            }

            Result.m_VertexBinding = vk::VertexInputBindingDescription{
                .binding   = Desc.VertexInputLayout.Binding,
                .stride    = Desc.VertexInputLayout.Stride,
                .inputRate = vk::VertexInputRate::eVertex,
            };
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
            .vertexBindingDescriptionCount   = 1,
            .pVertexBindingDescriptions      = &m_VertexBinding,
            .vertexAttributeDescriptionCount = static_cast<Uint32>(m_VertexAttributes.size()),
            .pVertexAttributeDescriptions    = m_VertexAttributes.data(),
        };
    }

  private:
    std::vector<vk::raii::ShaderModule>              m_Modules           = {};
    vk::VertexInputBindingDescription                m_VertexBinding     = {};
    std::vector<vk::VertexInputAttributeDescription> m_VertexAttributes  = {};
};

} // namespace SoulEngine::RHI::Vulkan
