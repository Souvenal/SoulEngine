/// @file   SlangCompiler/SlangReflection.cppm
/// @brief  Slang shader reflection extraction — normalized bindings, push
///         constants, vertex inputs.
///
/// Stateless extraction functions that translate Slang reflection API objects
/// into the engine's uniform ShaderReflection representation.
/// All Slang SDK dependencies are isolated to this partition.

module;

#include <slang.h>

export module Slang:Reflection;

import :Types;
import :Utils;

import std;
import Core;
import Shader;

namespace SoulEngine {

namespace {

constexpr auto UnknownBindingIndex = static_cast<unsigned>(SLANG_UNKNOWN_SIZE);

// ── Reflection extraction helpers ──────────────────────────────────
// File-local helpers for normalizing Slang reflection details before the
// Slang module exposes the final ShaderReflection value.

[[nodiscard]] auto IsParameterBlockTypeLayout(slang::TypeLayoutReflection* TypeLayout) -> bool {
    while (TypeLayout && TypeLayout->getKind() == slang::TypeReflection::Kind::Array)
        TypeLayout = TypeLayout->getElementTypeLayout();
    return TypeLayout && TypeLayout->getKind() == slang::TypeReflection::Kind::ParameterBlock;
}

[[nodiscard]] auto ExtractArrayCount(slang::ShaderReflection* ProgramLayout,
                                     slang::TypeLayoutReflection* TypeLayout,
                                     StringView ParameterName) -> std::expected<Uint32, ErrorMessage> {
    if (!TypeLayout || !TypeLayout->isArray())
        return 1U;

    // Slang needs the full program layout to resolve array element counts,
    // especially for arrays whose size is derived after composition/linking.
    const auto ElementCount = TypeLayout->getElementCount(ProgramLayout);
    if (ElementCount == SLANG_UNKNOWN_SIZE) {
        return std::unexpected(
            ErrorMessage(Format("Shader parameter '{}' has an unsupported array count in reflection", ParameterName)));
    }

    // Runtime-sized descriptor arrays are represented with a sentinel here.
    // The Vulkan backend must lower that sentinel to the actual bindless table
    // capacity instead of using it directly as a descriptor count.
    const bool bUnboundedArray = ElementCount == 0 || ElementCount == SLANG_UNBOUNDED_SIZE ||
                                 ElementCount == static_cast<size_t>(std::numeric_limits<Int32>::max());
    return bUnboundedArray ? std::numeric_limits<Uint32>::max() : static_cast<Uint32>(ElementCount);
}

[[nodiscard]] auto ExtractParameterBlockBindings(slang::ShaderReflection*          ProgramLayout,
                                                 slang::VariableLayoutReflection* Param)
    -> std::expected<std::vector<ShaderBinding>, ErrorMessage> {
    std::vector<ShaderBinding> Bindings;
    if (!Param)
        return Bindings;

    auto* TypeLayout = Param->getTypeLayout();
    if (!TypeLayout || TypeLayout->getKind() != slang::TypeReflection::Kind::ParameterBlock)
        return Bindings;

    // A ParameterBlock is reflected as one shader parameter whose descriptor
    // set is on the outer parameter, while concrete bindings live on fields of
    // the block element struct.  Emit one engine binding per field so host code
    // can bind resources by stable paths such as "g_frame.cb".
    const String ParameterName = Param->getName() ? String(Param->getName()) : String("<unnamed>");
    const auto   Set          = Param->getBindingIndex();
    if (Set == UnknownBindingIndex) {
        return std::unexpected(
            ErrorMessage(Format("Shader parameter '{}' is missing a concrete descriptor set location", ParameterName)));
    }

    auto* ElementLayout = TypeLayout->getElementVarLayout();
    auto* ElementType   = ElementLayout ? ElementLayout->getTypeLayout() : nullptr;
    if (!ElementType || ElementType->getKind() != slang::TypeReflection::Kind::Struct) {
        return std::unexpected(
            ErrorMessage(Format("ParameterBlock '{}' is missing a reflected element struct", ParameterName)));
    }

    for (unsigned int FieldIndex = 0; FieldIndex < ElementType->getFieldCount(); ++FieldIndex) {
        auto* Field = ElementType->getFieldByIndex(FieldIndex);
        if (!Field)
            continue;

        auto* ResourceTypeLayout = Field->getTypeLayout();
        if (!ResourceTypeLayout)
            continue;

        if (IsParameterBlockTypeLayout(ResourceTypeLayout)) {
            return std::unexpected(ErrorMessage(Format(
                "Nested ParameterBlock '{}.{}' is not supported; flatten shader parameter groups instead",
                ParameterName,
                Field->getName() ? Field->getName() : "<unnamed>")));
        }

        const auto BindingIndex = Field->getBindingIndex();
        if (BindingIndex == UnknownBindingIndex) {
            return std::unexpected(ErrorMessage(Format(
                "ParameterBlock field '{}.{}' is missing a concrete descriptor binding location",
                ParameterName,
                Field->getName() ? Field->getName() : "<unnamed>")));
        }

        auto ArrayCount = ExtractArrayCount(ProgramLayout, ResourceTypeLayout, ParameterName);
        if (!ArrayCount)
            return std::unexpected(std::move(ArrayCount.error()));

        const auto ResourceType = ToShaderResourceType(slang::BindingType::ParameterBlock, ResourceTypeLayout);
        if (!ResourceType) {
            return std::unexpected(ResourceType.error().Append(
                Format("Failed to normalize shader parameter '{}.{}'",
                       ParameterName,
                       Field->getName() ? Field->getName() : "<unnamed>")));
        }

        Bindings.emplace_back(ShaderBinding{
            .ParameterPath = Format("{}.{}", ParameterName, Field->getName() ? Field->getName() : "<unnamed>"),
            .Set           = static_cast<Uint32>(Set),
            .BindingIndex  = static_cast<Uint32>(BindingIndex),
            .Type          = *ResourceType,
            .ArrayCount    = *ArrayCount,
        });
    }

    return Bindings;
}

[[nodiscard]] auto ExtractShaderBindings(slang::ShaderReflection* ProgramLayout)
    -> std::expected<std::vector<ShaderBinding>, ErrorMessage> {
    std::vector<ShaderBinding> Bindings;
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

        // ParameterBlock is the common path for logical constant/resource
        // groups.  Handle it explicitly before the generic binding-range path
        // because the host-visible binding path should name the block field,
        // not just the outer block object.
        if (TypeLayout->getKind() == slang::TypeReflection::Kind::ParameterBlock) {
            auto ParameterBlockBindings = ExtractParameterBlockBindings(ProgramLayout, Param);
            if (!ParameterBlockBindings)
                return std::unexpected(std::move(ParameterBlockBindings.error()));
            Bindings.append_range(*ParameterBlockBindings);
            continue;
        }

        // A Slang binding range describes one contiguous descriptor allocation
        // owned by a shader parameter: resource kind, set/space, binding base,
        // and descriptor count.  Slang may split arrays, descriptor ranges, or
        // aggregate resources into multiple ranges, so this code emits the
        // flattened engine bindings that Vulkan layout creation consumes later.
        const auto RangeCount = TypeLayout->getBindingRangeCount();
        if (RangeCount == 0)
            continue;
        const auto BindingBase = Param->getBindingIndex();
        if (BindingBase == UnknownBindingIndex) {
            return std::unexpected(
                ErrorMessage(Format("Shader parameter '{}' is missing a concrete descriptor set/binding location",
                                    Param->getName() ? Param->getName() : "<unnamed>")));
        }

        Uint32 BindingCursor = 0;
        // BindingCursor tracks how many descriptor slots previous ranges used
        // when Slang gives only a binding base on the outer parameter.
        for (SlangInt RangeIndex = 0; RangeIndex < RangeCount; ++RangeIndex) {
            const auto BindingType = TypeLayout->getBindingRangeType(RangeIndex);
            if (BindingType == slang::BindingType::ParameterBlock) {
                return std::unexpected(ErrorMessage(Format(
                    "ParameterBlock shader parameter '{}' must use the explicit ParameterBlock reflection path",
                    Param->getName() ? Param->getName() : "<unnamed>")));
            }

            // This is the number of descriptor slots occupied by this reflected
            // range after Slang lowers the source type to the target layout.
            // Resource arrays commonly make this greater than one; aggregate
            // resource structs can also contribute multiple slots.
            const auto BindingCount = TypeLayout->getBindingRangeBindingCount(RangeIndex);
            if (BindingCount == SLANG_UNKNOWN_SIZE) {
                return std::unexpected(
                    ErrorMessage(Format("Shader parameter '{}' has an unsupported descriptor count in reflection",
                                        Param->getName() ? Param->getName() : "<unnamed>")));
            }

            // Slang's binding-range descriptor-set index is an internal index
            // into the reflected descriptor-set list, not the Vulkan set number.
            // The parameter binding space is the HLSL space / Vulkan set.
            const auto Set = Param->getBindingSpace();
            if (Set == UnknownBindingIndex) {
                return std::unexpected(
                    ErrorMessage(Format("Shader parameter '{}' is missing a concrete descriptor set location",
                                        Param->getName() ? Param->getName() : "<unnamed>")));
            }

            const auto DescriptorRangeCount = TypeLayout->getBindingRangeDescriptorRangeCount(RangeIndex);
            // Multi-descriptor ranges are expanded into separate Binding
            // records when each descriptor has a distinct binding number.
            // True arrays stay as one Binding with ArrayCount > 1.
            const bool   bExpandBindings    = DescriptorRangeCount != SLANG_UNKNOWN_SIZE && DescriptorRangeCount > 1;
            const Uint32 OutputBindingCount = bExpandBindings ? static_cast<Uint32>(DescriptorRangeCount) : 1;
            const Uint32 OutputArrayCount   = bExpandBindings ? 1 : static_cast<Uint32>(BindingCount);

            for (Uint32 BindingOffset = 0; BindingOffset < OutputBindingCount; ++BindingOffset) {
                auto*  ResourceTypeLayout = TypeLayout->getBindingRangeLeafTypeLayout(RangeIndex);
                auto   BindingIndex       = static_cast<Uint32>(BindingBase) + BindingCursor + BindingOffset;
                String ParameterPath      = Param->getName() ? String(Param->getName()) : String{};

                const auto ResourceType = ToShaderResourceType(BindingType, ResourceTypeLayout);
                if (!ResourceType) {
                    return std::unexpected(ResourceType.error().Append(
                        Format("Failed to normalize shader parameter '{}'",
                               Param->getName() ? Param->getName() : "<unnamed>")));
                }

                Bindings.emplace_back(ShaderBinding{
                    .ParameterPath = std::move(ParameterPath),
                    .Set           = static_cast<Uint32>(Set),
                    .BindingIndex  = BindingIndex,
                    .Type          = *ResourceType,
                    .ArrayCount    = OutputArrayCount,
                });
            }
            BindingCursor += bExpandBindings ? OutputBindingCount : static_cast<Uint32>(BindingCount);
        }
    }

    return Bindings;
}

[[nodiscard]] auto AppendPushConstantRange(std::vector<ShaderPushConstantRange>& PushConstants,
                                           slang::VariableLayoutReflection*        Param,
                                           slang::ParameterCategory                Category)
    -> std::expected<void, ErrorMessage> {
    if (!Param || !HasCategory(Param, Category))
        return {};

    auto* TypeLayout = Param->getTypeLayout();
    if (!TypeLayout)
        return {};

    // Push constants are not descriptor bindings.  Keep them as byte ranges
    // so the Vulkan backend can build pipeline-layout push-constant ranges.
    const auto Offset = Param->getOffset(Category);
    const auto Size   = TypeLayout->getSize(Category);
    if (Offset == SLANG_UNKNOWN_SIZE || Size == SLANG_UNKNOWN_SIZE || Size == SLANG_UNBOUNDED_SIZE) {
        return std::unexpected(
            ErrorMessage(Format("Push-constant parameter '{}' has an unsupported layout in reflection",
                                Param->getName() ? Param->getName() : "<unnamed>")));
    }

    PushConstants.emplace_back(ShaderPushConstantRange{
        .Offset = static_cast<Uint32>(Offset),
        .Size   = static_cast<Uint32>(Size),
    });
    return {};
}

[[nodiscard]] auto MergePushConstantRanges(std::vector<ShaderPushConstantRange> Ranges)
    -> std::vector<ShaderPushConstantRange> {
    std::ranges::sort(Ranges, {}, &ShaderPushConstantRange::Offset);

    std::vector<ShaderPushConstantRange> Merged;
    for (const auto& Range : Ranges) {
        if (Range.Size == 0)
            continue;

        if (Merged.empty()) {
            Merged.push_back(Range);
            continue;
        }

        auto&      Last    = Merged.back();
        const auto LastEnd = Last.Offset + Last.Size;
        const auto NextEnd = Range.Offset + Range.Size;
        if (Range.Offset <= LastEnd) {
            Last.Size = (std::max)(LastEnd, NextEnd) - Last.Offset;
            continue;
        }

        Merged.push_back(Range);
    }

    return Merged;
}

[[nodiscard]] auto ExtractPushConstantRanges(slang::ShaderReflection*     ProgramLayout,
                                             slang::EntryPointReflection* VertexEntryPoint,
                                             slang::EntryPointReflection* FragmentEntryPoint)
    -> std::expected<std::vector<ShaderPushConstantRange>, ErrorMessage> {
    std::vector<ShaderPushConstantRange> PushConstants;
    if (!ProgramLayout)
        return PushConstants;

    for (unsigned Index = 0; Index < ProgramLayout->getParameterCount(); ++Index) {
        if (auto R = AppendPushConstantRange(PushConstants,
                                             ProgramLayout->getParameterByIndex(Index),
                                             slang::ParameterCategory::PushConstantBuffer);
            !R) {
            return std::unexpected(std::move(R.error()));
        }
    }

    for (auto* EntryPoint : {VertexEntryPoint, FragmentEntryPoint}) {
        if (!EntryPoint)
            continue;
        for (unsigned Index = 0; Index < EntryPoint->getParameterCount(); ++Index) {
            // Slang lowers ordinary-data `uniform` entry-point parameters to
            // SPIR-V push constants. Reflection reports those source parameters
            // under the Uniform category, while explicitly attributed global
            // push-constant parameters use PushConstantBuffer above.
            if (auto R = AppendPushConstantRange(PushConstants,
                                                 EntryPoint->getParameterByIndex(Index),
                                                 slang::ParameterCategory::Uniform);
                !R) {
                return std::unexpected(std::move(R.error()));
            }
        }
    }

    return MergePushConstantRanges(std::move(PushConstants));
}

[[nodiscard]] auto ExtractVertexInputsFromVarLayout(slang::VariableLayoutReflection*           VarLayout,
                                                    std::vector<ShaderVertexInputAttribute>& VertexInputs)
    -> std::expected<void, ErrorMessage> {
    if (!VarLayout)
        return {};

    auto* TypeLayout = VarLayout->getTypeLayout();
    if (!TypeLayout)
        return {};

    // Entry-point inputs are often reflected as a single struct parameter.
    // Flatten it into individual vertex attributes so RHI vertex input
    // validation can reason about locations and formats directly.
    if (TypeLayout->getKind() == slang::TypeReflection::Kind::Struct) {
        for (unsigned int FieldIndex = 0; FieldIndex < TypeLayout->getFieldCount(); ++FieldIndex) {
            if (auto R = ExtractVertexInputsFromVarLayout(TypeLayout->getFieldByIndex(FieldIndex), VertexInputs); !R) {
                return std::unexpected(std::move(R.error()));
            }
        }
        return {};
    }

    StringView SemanticName = VarLayout->getSemanticName() ? StringView(VarLayout->getSemanticName()) : StringView{};
    // System-value semantics such as SV_VertexID are generated by the pipeline
    // and must not be matched against CPU vertex-buffer layout.
    if (IsSystemValueSemantic(SemanticName))
        return {};

    std::optional<Uint32> Location            = std::nullopt;
    const auto            BindingIndex        = VarLayout->getBindingIndex();
    if (BindingIndex != UnknownBindingIndex)
        Location = static_cast<Uint32>(BindingIndex);

    VertexInputs.emplace_back(ShaderVertexInputAttribute{
        .SemanticName  = String(SemanticName),
        .SemanticIndex = static_cast<Uint32>(VarLayout->getSemanticIndex()),
        .Location      = Location,
        .ValueType     = ToShaderValueType(TypeLayout),
    });
    return {};
}

[[nodiscard]] auto ExtractVertexInputs(slang::EntryPointReflection* EntryPoint)
    -> std::expected<std::vector<ShaderVertexInputAttribute>, ErrorMessage> {
    std::vector<ShaderVertexInputAttribute> VertexInputs;
    // Only vertex entry points consume fixed-function vertex inputs.
    if (!EntryPoint || EntryPoint->getStage() != SLANG_STAGE_VERTEX)
        return VertexInputs;

    for (unsigned Index = 0; Index < EntryPoint->getParameterCount(); ++Index) {
        if (auto R = ExtractVertexInputsFromVarLayout(EntryPoint->getParameterByIndex(Index), VertexInputs); !R) {
            return std::unexpected(std::move(R.error()));
        }
    }

    return VertexInputs;
}

} // anonymous namespace

[[nodiscard]] auto BuildShaderReflection(slang::ShaderReflection*     ProgramLayout,
                                         slang::EntryPointReflection* VertexEntryPoint,
                                         slang::EntryPointReflection* FragmentEntryPoint)
    -> std::expected<ShaderReflection, ErrorMessage> {
    if (!ProgramLayout || !VertexEntryPoint || !FragmentEntryPoint)
        return std::unexpected(ErrorMessage("Shader reflection is incomplete for a compiled entry point"));

    // Bindings and push constants come from the linked program layout so they
    // include all graphics stages.  Vertex inputs come specifically from the
    // vertex entry point because fragment inputs are interpolants, not CPU
    // vertex-buffer attributes.
    auto Bindings = ExtractShaderBindings(ProgramLayout);
    if (!Bindings)
        return std::unexpected(Bindings.error());

    auto PushConstants = ExtractPushConstantRanges(ProgramLayout, VertexEntryPoint, FragmentEntryPoint);
    if (!PushConstants)
        return std::unexpected(PushConstants.error());

    auto VertexInputs = ExtractVertexInputs(VertexEntryPoint);
    if (!VertexInputs)
        return std::unexpected(VertexInputs.error());

    return ShaderReflection{
        .Bindings      = std::move(*Bindings),
        .PushConstants = std::move(*PushConstants),
        .VertexInputs  = std::move(*VertexInputs),
    };
}

[[nodiscard]] auto BuildRayTracingShaderReflection(
    slang::ShaderReflection* ProgramLayout, std::span<slang::EntryPointReflection* const> EntryPoints)
    -> std::expected<ShaderReflection, ErrorMessage> {
    if (!ProgramLayout || EntryPoints.empty())
        return std::unexpected(ErrorMessage("Ray-tracing shader reflection is incomplete for a compiled entry point"));

    auto Bindings = ExtractShaderBindings(ProgramLayout);
    if (!Bindings)
        return std::unexpected(Bindings.error());

    std::vector<ShaderPushConstantRange> PushConstants = {};
    for (unsigned Index = 0; Index < ProgramLayout->getParameterCount(); ++Index) {
        if (auto R = AppendPushConstantRange(PushConstants,
                                             ProgramLayout->getParameterByIndex(Index),
                                             slang::ParameterCategory::PushConstantBuffer);
            !R) {
            return std::unexpected(std::move(R.error()));
        }
    }
    for (auto* EntryPoint : EntryPoints) {
        if (!EntryPoint)
            return std::unexpected(ErrorMessage("Ray-tracing reflection contains a null entry point"));
        for (unsigned Index = 0; Index < EntryPoint->getParameterCount(); ++Index) {
            if (auto R = AppendPushConstantRange(PushConstants,
                                                 EntryPoint->getParameterByIndex(Index),
                                                 slang::ParameterCategory::Uniform);
                !R) {
                return std::unexpected(std::move(R.error()));
            }
        }
    }

    return ShaderReflection{
        .Bindings      = std::move(*Bindings),
        .PushConstants = MergePushConstantRanges(std::move(PushConstants)),
        .VertexInputs  = {},
    };
}

} // namespace SoulEngine
