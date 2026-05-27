/// @file   SlangCompiler/SlangReflection.cppm
/// @brief  Slang shader reflection extraction — normalized bindings, push
///         constants, vertex inputs.
///
/// Stateless extraction functions that translate Slang reflection API objects
/// into the engine's uniform Shader::Reflection representation.
/// All Slang SDK dependencies are isolated to this partition.

module;

#include <slang.h>

export module Slang:Reflection;

import :Types;
import :Utils;

import std;
import Core;
import Shader;

namespace SoulEngine::ShaderCompiler::SlangCompiler {

using namespace SoulEngine::Core;

// ── Reflection extraction helpers ──────────────────────────────────
// These are module-internal (not exported) — visible within the Slang
// module but not to importers of ShaderCompiler.

[[nodiscard]] auto ExtractShaderBindings(slang::ShaderReflection* ProgramLayout)
    -> std::expected<std::vector<Shader::Binding>, ErrorMessage> {
    std::vector<Shader::Binding> Bindings;
    if (!ProgramLayout)
        return Bindings;

    for (unsigned Index = 0; Index < ProgramLayout->getParameterCount(); ++Index) {
        auto* Param = ProgramLayout->getParameterByIndex(Index);
        if (!Param)
            continue;
        if (HasCategory(Param, slang::ParameterCategory::PushConstantBuffer))
            continue;

        auto* TypeLayout = Param->getTypeLayout();
        if (!TypeLayout)
            continue;

        const auto RangeCount = TypeLayout->getBindingRangeCount();
        if (RangeCount == 0)
            continue;
        if (RangeCount != 1) {
            return std::unexpected(ErrorMessage(Format("Shader parameter '{}' uses {} binding ranges; only one is "
                                                       "supported in the initial normalized reflection",
                                                       Param->getName() ? Param->getName() : "<unnamed>",
                                                       RangeCount)));
        }

        const auto BindingCount = TypeLayout->getBindingRangeBindingCount(0);
        if (BindingCount == SLANG_UNKNOWN_SIZE || BindingCount == SLANG_UNBOUNDED_SIZE) {
            return std::unexpected(
                ErrorMessage(Format("Shader parameter '{}' has an unsupported descriptor count in reflection",
                                    Param->getName() ? Param->getName() : "<unnamed>")));
        }

        const auto ResourceType = ToShaderResourceType(TypeLayout->getBindingRangeType(0), TypeLayout);
        if (!ResourceType)
            return std::unexpected(ResourceType.error().Append(Format(
                "Failed to normalize shader parameter '{}'", Param->getName() ? Param->getName() : "<unnamed>")));

        const auto     Set                 = TypeLayout->getBindingRangeDescriptorSetIndex(0);
        const auto     Binding             = Param->getBindingIndex();
        constexpr auto UnknownBindingIndex = static_cast<unsigned>(SLANG_UNKNOWN_SIZE);
        if (Set < 0 || Binding == UnknownBindingIndex) {
            return std::unexpected(
                ErrorMessage(Format("Shader parameter '{}' is missing a concrete descriptor set/binding location",
                                    Param->getName() ? Param->getName() : "<unnamed>")));
        }

        Bindings.push_back(Shader::Binding{
            .Set        = static_cast<Uint32>(Set),
            .Binding    = static_cast<Uint32>(Binding),
            .Type       = *ResourceType,
            .ArrayCount = static_cast<Uint32>(BindingCount),
        });
    }

    return Bindings;
}

[[nodiscard]] auto ExtractPushConstantRanges(slang::ShaderReflection* ProgramLayout)
    -> std::expected<std::vector<Shader::PushConstantRange>, ErrorMessage> {
    std::vector<Shader::PushConstantRange> PushConstants;
    if (!ProgramLayout)
        return PushConstants;

    for (unsigned Index = 0; Index < ProgramLayout->getParameterCount(); ++Index) {
        auto* Param = ProgramLayout->getParameterByIndex(Index);
        if (!Param || !HasCategory(Param, slang::ParameterCategory::PushConstantBuffer))
            continue;

        auto* TypeLayout = Param->getTypeLayout();
        if (!TypeLayout)
            continue;

        const auto Offset = Param->getOffset(slang::ParameterCategory::PushConstantBuffer);
        const auto Size   = TypeLayout->getSize(slang::ParameterCategory::PushConstantBuffer);
        if (Offset == SLANG_UNKNOWN_SIZE || Size == SLANG_UNKNOWN_SIZE || Size == SLANG_UNBOUNDED_SIZE) {
            return std::unexpected(
                ErrorMessage(Format("Push-constant parameter '{}' has an unsupported layout in reflection",
                                    Param->getName() ? Param->getName() : "<unnamed>")));
        }

        PushConstants.push_back(Shader::PushConstantRange{
            .Offset = static_cast<Uint32>(Offset),
            .Size   = static_cast<Uint32>(Size),
        });
    }

    return PushConstants;
}

[[nodiscard]] auto ExtractVertexInputsFromVarLayout(slang::VariableLayoutReflection*           VarLayout,
                                                    std::vector<Shader::VertexInputAttribute>& VertexInputs)
    -> std::expected<void, ErrorMessage> {
    if (!VarLayout)
        return {};

    auto* TypeLayout = VarLayout->getTypeLayout();
    if (!TypeLayout)
        return {};

    if (TypeLayout->getKind() == slang::TypeReflection::Kind::Struct) {
        for (unsigned int FieldIndex = 0; FieldIndex < TypeLayout->getFieldCount(); ++FieldIndex) {
            if (auto R = ExtractVertexInputsFromVarLayout(TypeLayout->getFieldByIndex(FieldIndex), VertexInputs); !R) {
                return std::unexpected(std::move(R.error()));
            }
        }
        return {};
    }

    StringView SemanticName = VarLayout->getSemanticName() ? StringView(VarLayout->getSemanticName()) : StringView{};
    if (IsSystemValueSemantic(SemanticName))
        return {};

    std::optional<Uint32> Location            = std::nullopt;
    const auto            BindingIndex        = VarLayout->getBindingIndex();
    constexpr auto        UnknownBindingIndex = static_cast<unsigned>(SLANG_UNKNOWN_SIZE);
    if (BindingIndex != UnknownBindingIndex)
        Location = static_cast<Uint32>(BindingIndex);

    VertexInputs.push_back(Shader::VertexInputAttribute{
        .SemanticName  = String(SemanticName),
        .SemanticIndex = static_cast<Uint32>(VarLayout->getSemanticIndex()),
        .Location      = Location,
        .ValueType     = ToShaderValueType(TypeLayout),
    });
    return {};
}

[[nodiscard]] auto ExtractVertexInputs(slang::EntryPointReflection* EntryPoint)
    -> std::expected<std::vector<Shader::VertexInputAttribute>, ErrorMessage> {
    std::vector<Shader::VertexInputAttribute> VertexInputs;
    if (!EntryPoint || EntryPoint->getStage() != SLANG_STAGE_VERTEX)
        return VertexInputs;

    for (unsigned Index = 0; Index < EntryPoint->getParameterCount(); ++Index) {
        if (auto R = ExtractVertexInputsFromVarLayout(EntryPoint->getParameterByIndex(Index), VertexInputs); !R) {
            return std::unexpected(std::move(R.error()));
        }
    }

    return VertexInputs;
}

[[nodiscard]] auto BuildShaderReflection(slang::ShaderReflection*     ProgramLayout,
                                         slang::EntryPointReflection* EntryPoint)
    -> std::expected<SPtr<const Shader::Reflection>, ErrorMessage> {
    if (!ProgramLayout || !EntryPoint)
        return std::unexpected(ErrorMessage("Shader reflection is incomplete for a compiled entry point"));

    auto Bindings = ExtractShaderBindings(ProgramLayout);
    if (!Bindings)
        return std::unexpected(Bindings.error());

    auto PushConstants = ExtractPushConstantRanges(ProgramLayout);
    if (!PushConstants)
        return std::unexpected(PushConstants.error());

    auto VertexInputs = ExtractVertexInputs(EntryPoint);
    if (!VertexInputs)
        return std::unexpected(VertexInputs.error());

    SPtr<const Shader::Reflection> Reflection = std::make_shared<Shader::Reflection>(Shader::Reflection{
        .Bindings      = std::move(*Bindings),
        .PushConstants = std::move(*PushConstants),
        .VertexInputs  = std::move(*VertexInputs),
    });
    return Reflection;
}

} // namespace SoulEngine::ShaderCompiler::SlangCompiler
