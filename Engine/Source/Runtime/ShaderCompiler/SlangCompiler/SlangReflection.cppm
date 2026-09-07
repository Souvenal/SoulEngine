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

// ── Reflection DFS ───────────────────────────────────────────────────
// The whole program interface is collected by one depth-first walk over the
// Slang variable tree.  Two orthogonal dimensions meet at every node:
//
//   * Location coordinates accumulate along the path.  Slang reports every
//     variable's binding space / binding index relative to its enclosing
//     scope, so the walk accumulates a cursor: space -> descriptor set, slot
//     -> binding, PushConstantBuffer / Uniform categories -> byte offsets for
//     push constants.  A ParameterBlock is the space-boundary case: the block
//     variable reports its set through its binding index, and its fields
//     re-bind from slot zero inside that set.
//   * The type-layout kind drives recursion: containers (ParameterBlock,
//     struct) descend into their children, resource leaves emit bindings.
//
// Push constants and descriptor bindings are therefore emitted by the same
// walk instead of two separate extraction passes.

struct ReflectionCursor {
    String Path        = {};
    size_t Set         = 0; // binding-space axis
    size_t BindingBase = 0; // descriptor-slot axis
    size_t PushBase    = 0; // PushConstantBuffer axis (bytes)
    size_t UniformBase = 0; // Uniform axis (bytes)
};

struct ReflectionSinks {
    slang::ShaderReflection*              ProgramLayout = nullptr;
    std::vector<ShaderBinding>*           Bindings      = nullptr;
    std::vector<ShaderPushConstantRange>* PushConstants = nullptr;
};

enum class ReflectionVisitMode {
    Full,             // global-scope parameters: bindings + push constants
    PushConstantsOnly // entry-point parameters: push constants only
};

constexpr auto UnknownBindingIndex = static_cast<unsigned>(SLANG_UNKNOWN_SIZE);

[[nodiscard]] auto AppendPath(const String& Prefix, const char* Name) -> String {
    const String Segment = Name ? String(Name) : String("<unnamed>");
    return Prefix.empty() ? Segment : Format("{}.{}", Prefix, Segment);
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

    // Normalize runtime-array sentinels into the backend-independent Shader
    // reflection contract. Slang documents SLANG_UNBOUNDED_SIZE, but the
    // current SDK can return INT32_MAX for unsized descriptor arrays.
    if (ElementCount == SLANG_UNBOUNDED_SIZE ||
        ElementCount == static_cast<size_t>(std::numeric_limits<Int32>::max()))
        return kShaderReflectionArrayUnboundedSize;
    return static_cast<Uint32>(ElementCount);
}

[[nodiscard]] auto EmitPushConstantRange(slang::VariableLayoutReflection* Var,
                                         size_t                          Base,
                                         slang::ParameterCategory        Category,
                                         const String&                   Path,
                                         ReflectionSinks&                Sinks) -> std::expected<void, ErrorMessage> {
    auto* TypeLayout = Var->getTypeLayout();
    if (!TypeLayout)
        return {};

    // Push constants are not descriptor bindings.  Keep them as byte ranges
    // so the Vulkan backend can build pipeline-layout push-constant ranges.
    const auto Offset = Var->getOffset(Category);
    const auto Size   = TypeLayout->getSize(Category);
    if (Offset == SLANG_UNKNOWN_SIZE || Size == SLANG_UNKNOWN_SIZE || Size == SLANG_UNBOUNDED_SIZE) {
        return std::unexpected(
            ErrorMessage(Format("Push-constant parameter '{}' has an unsupported layout in reflection", Path)));
    }

    Sinks.PushConstants->push_back(ShaderPushConstantRange{
        .Offset = static_cast<Uint32>(Base + Offset),
        .Size   = static_cast<Uint32>(Size),
    });
    return {};
}

[[nodiscard]] auto VisitVariable(slang::VariableLayoutReflection* Var,
                                 const ReflectionCursor&          Parent,
                                 ReflectionSinks&                 Sinks,
                                 ReflectionVisitMode              Mode,
                                 bool                             bAllowParameterBlock)
    -> std::expected<void, ErrorMessage> {
    if (!Var)
        return {};

    const String Path = AppendPath(Parent.Path, Var->getName());

    // Push constants live on the category axis, independent of the type kind.
    // Slang lowers ordinary-data `uniform` entry-point parameters to SPIR-V
    // push constants and reports them under the Uniform category, while
    // explicitly attributed global push-constant parameters use
    // PushConstantBuffer.  A variable that occupies push-constant space holds
    // ordinary data only, so the walk never descends past it.
    if (Mode == ReflectionVisitMode::PushConstantsOnly) {
        if (!HasCategory(Var, slang::ParameterCategory::Uniform))
            return {};
        return EmitPushConstantRange(Var, Parent.UniformBase, slang::ParameterCategory::Uniform, Path, Sinks);
    }
    if (HasCategory(Var, slang::ParameterCategory::PushConstantBuffer)) {
        return EmitPushConstantRange(Var, Parent.PushBase, slang::ParameterCategory::PushConstantBuffer, Path, Sinks);
    }

    auto* TypeLayout = Var->getTypeLayout();
    if (!TypeLayout)
        return {};

    // Slang reports a variable's register-space (set) and descriptor-slot
    // (binding) coordinates relative to its enclosing scope; both come back
    // as UnknownBindingIndex when the variable does not occupy that axis.
    const auto SpaceIndex = Var->getBindingSpace();
    const auto SlotIndex  = Var->getBindingIndex();

    // A descriptor array occupies one binding as a whole, so array layers are
    // peeled only to classify the element kind; the count is extracted from
    // the un-stripped layout at leaf emission.
    auto* LeafLayout = StripArrayTypeLayout(TypeLayout);
    if (!LeafLayout)
        return {};

    switch (LeafLayout->getKind()) {
    case slang::TypeReflection::Kind::ParameterBlock: {
        // A ParameterBlock owns an entire descriptor set: the block variable
        // carries the set on the RegisterSpace axis and its element-struct
        // fields bind from slot zero inside that set.
        if (!bAllowParameterBlock) {
            return std::unexpected(ErrorMessage(Format(
                "Nested ParameterBlock '{}' is not supported; flatten shader parameter groups instead",
                Path)));
        }
        // A ParameterBlock reports its descriptor set through its binding
        // index rather than a register-space coordinate.
        if (SlotIndex == UnknownBindingIndex) {
            return std::unexpected(
                ErrorMessage(Format("Shader parameter '{}' is missing a concrete descriptor set location", Path)));
        }
        auto* ElementLayout = LeafLayout->getElementVarLayout();
        auto* ElementType   = ElementLayout ? ElementLayout->getTypeLayout() : nullptr;
        if (!ElementType || ElementType->getKind() != slang::TypeReflection::Kind::Struct) {
            return std::unexpected(
                ErrorMessage(Format("ParameterBlock '{}' is missing a reflected element struct", Path)));
        }
        const ReflectionCursor BlockScope{
            .Path        = Path,
            .Set         = Parent.Set + SlotIndex,
            .BindingBase = 0,
            .PushBase    = Parent.PushBase,
            .UniformBase = Parent.UniformBase,
        };
        for (unsigned int FieldIndex = 0; FieldIndex < ElementType->getFieldCount(); ++FieldIndex) {
            if (auto R = VisitVariable(ElementType->getFieldByIndex(FieldIndex),
                                       BlockScope,
                                       Sinks,
                                       ReflectionVisitMode::Full,
                                       /*bAllowParameterBlock=*/false);
                !R)
                return R;
        }
        return {};
    }
    case slang::TypeReflection::Kind::Struct: {
        // A plain struct is a resource facade or aggregate (e.g.
        // GeometryRecordTable): its descriptor bindings live on the inner
        // fields, so descend instead of treating the struct itself as a leaf.
        if (TypeLayout->isArray()) {
            return std::unexpected(ErrorMessage(Format(
                "Shader parameter '{}' is an array of structs with resource members, which is not supported",
                Path)));
        }
        const ReflectionCursor StructScope{
            .Path        = Path,
            .Set         = SpaceIndex != UnknownBindingIndex ? Parent.Set + SpaceIndex : Parent.Set,
            .BindingBase = SlotIndex != UnknownBindingIndex ? Parent.BindingBase + SlotIndex : Parent.BindingBase,
            .PushBase    = Parent.PushBase,
            .UniformBase = Parent.UniformBase,
        };
        for (unsigned int FieldIndex = 0; FieldIndex < LeafLayout->getFieldCount(); ++FieldIndex) {
            if (auto R = VisitVariable(LeafLayout->getFieldByIndex(FieldIndex),
                                       StructScope,
                                       Sinks,
                                       ReflectionVisitMode::Full,
                                       /*bAllowParameterBlock=*/false);
                !R)
                return R;
        }
        return {};
    }
    default: {
        // Ordinary data (scalars, vectors, matrices) occupies no descriptor
        // slot; it lives in an implicit constant buffer and never becomes an
        // engine binding.
        if (!HasCategory(Var, slang::ParameterCategory::DescriptorTableSlot))
            return {};
        if (SlotIndex == UnknownBindingIndex) {
            return std::unexpected(ErrorMessage(
                Format("Shader parameter '{}' is missing a concrete descriptor binding location", Path)));
        }

        auto ArrayCount = ExtractArrayCount(Sinks.ProgramLayout, TypeLayout, Path);
        if (!ArrayCount)
            return std::unexpected(std::move(ArrayCount.error()));

        auto ResourceType = ToShaderResourceTypeFromLeafLayout(LeafLayout);
        if (!ResourceType) {
            return std::unexpected(
                ResourceType.error().Append(Format("Failed to normalize shader parameter '{}'", Path)));
        }

        Sinks.Bindings->emplace_back(ShaderBinding{
            .ParameterPath = Path,
            .Set           = static_cast<Uint32>(SpaceIndex != UnknownBindingIndex ? Parent.Set + SpaceIndex
                                                                                   : Parent.Set),
            .BindingIndex  = static_cast<Uint32>(Parent.BindingBase + SlotIndex),
            .Type          = *ResourceType,
            .ArrayCount    = *ArrayCount,
        });
        return {};
    }
    }
}

[[nodiscard]] auto CollectProgramInterface(slang::ShaderReflection*                       ProgramLayout,
                                           std::span<slang::EntryPointReflection* const> EntryPoints,
                                           std::vector<ShaderBinding>&                    Bindings,
                                           std::vector<ShaderPushConstantRange>&          PushConstants)
    -> std::expected<void, ErrorMessage> {
    if (!ProgramLayout)
        return {};

    ReflectionSinks Sinks{
        .ProgramLayout = ProgramLayout,
        .Bindings      = &Bindings,
        .PushConstants = &PushConstants,
    };

    // Global parameters carry every descriptor binding plus explicitly
    // attributed push-constant blocks.
    for (unsigned Index = 0; Index < ProgramLayout->getParameterCount(); ++Index) {
        if (auto R = VisitVariable(ProgramLayout->getParameterByIndex(Index),
                                   ReflectionCursor{},
                                   Sinks,
                                   ReflectionVisitMode::Full,
                                   /*bAllowParameterBlock=*/true);
            !R)
            return R;
    }

    for (auto* EntryPoint : EntryPoints) {
        if (!EntryPoint)
            return std::unexpected(ErrorMessage("Shader reflection contains a null entry point"));
        for (unsigned Index = 0; Index < EntryPoint->getParameterCount(); ++Index) {
            if (auto R = VisitVariable(EntryPoint->getParameterByIndex(Index),
                                       ReflectionCursor{},
                                       Sinks,
                                       ReflectionVisitMode::PushConstantsOnly,
                                       /*bAllowParameterBlock=*/false);
                !R)
                return R;
        }
    }
    return {};
}

[[nodiscard]] auto ExtractBindlessSpace(slang::ShaderReflection* ProgramLayout)
    -> std::optional<Uint32> {
    if (!ProgramLayout)
        return std::nullopt;
    const auto Space = ProgramLayout->getBindlessSpaceIndex();
    if (Space == SLANG_UNKNOWN_SIZE)
        return std::nullopt;
    return static_cast<Uint32>(Space);
}

[[nodiscard]] auto MergePushConstantRanges(std::vector<ShaderPushConstantRange> Ranges)
    -> std::vector<ShaderPushConstantRange> {
    std::sort(Ranges.begin(),
              Ranges.end(),
              [](const ShaderPushConstantRange& Left, const ShaderPushConstantRange& Right) -> bool {
                  return Left.Offset < Right.Offset;
              });

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

[[nodiscard]] auto ExtractVertexInputsFromVarLayout(slang::VariableLayoutReflection*           VarLayout,
                                                    std::vector<ShaderVertexInputAttribute>& VertexInputs)
    -> std::expected<void, ErrorMessage> {
    if (!VarLayout || HasCategory(VarLayout, slang::ParameterCategory::Uniform))
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
    if (BindingIndex != static_cast<unsigned>(SLANG_UNKNOWN_SIZE))
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

[[nodiscard]] auto BuildGraphicsShaderReflection(slang::ShaderReflection*     ProgramLayout,
                                                 slang::EntryPointReflection* VertexEntryPoint,
                                                 slang::EntryPointReflection* FragmentEntryPoint)
    -> std::expected<ShaderReflection, ErrorMessage> {
    if (!ProgramLayout || !VertexEntryPoint || !FragmentEntryPoint)
        return std::unexpected(ErrorMessage("Shader reflection is incomplete for a compiled entry point"));

    // Bindings and push constants come from the linked program layout so they
    // include all graphics stages.  Vertex inputs come specifically from the
    // vertex entry point because fragment inputs are interpolants, not CPU
    // vertex-buffer attributes.
    std::vector<ShaderBinding>           Bindings;
    std::vector<ShaderPushConstantRange> PushConstants;
    const std::array                     EntryPoints{VertexEntryPoint, FragmentEntryPoint};
    if (auto R = CollectProgramInterface(ProgramLayout, EntryPoints, Bindings, PushConstants); !R)
        return std::unexpected(std::move(R.error()));

    auto VertexInputs = ExtractVertexInputs(VertexEntryPoint);
    if (!VertexInputs)
        return std::unexpected(VertexInputs.error());

    return ShaderReflection{
        .Bindings      = std::move(Bindings),
        .PushConstants = MergePushConstantRanges(std::move(PushConstants)),
        .VertexInputs  = std::move(*VertexInputs),
        .BindlessSpace = ExtractBindlessSpace(ProgramLayout),
    };
}

[[nodiscard]] auto BuildComputeShaderReflection(slang::ShaderReflection*     ProgramLayout,
                                                slang::EntryPointReflection* ComputeEntryPoint)
    -> std::expected<ShaderReflection, ErrorMessage> {
    if (!ProgramLayout || !ComputeEntryPoint)
        return std::unexpected(ErrorMessage("Shader reflection is incomplete for a compiled entry point"));

    std::vector<ShaderBinding>           Bindings;
    std::vector<ShaderPushConstantRange> PushConstants;
    const std::array                     EntryPoints{ComputeEntryPoint};
    if (auto R = CollectProgramInterface(ProgramLayout, EntryPoints, Bindings, PushConstants); !R)
        return std::unexpected(std::move(R.error()));

    return ShaderReflection{
        .Bindings      = std::move(Bindings),
        .PushConstants = MergePushConstantRanges(std::move(PushConstants)),
        .VertexInputs  = {},
        .BindlessSpace = ExtractBindlessSpace(ProgramLayout),
    };
}

[[nodiscard]] auto BuildRayTracingShaderReflection(
    slang::ShaderReflection* ProgramLayout, std::span<slang::EntryPointReflection* const> EntryPoints)
    -> std::expected<ShaderReflection, ErrorMessage> {
    if (!ProgramLayout || EntryPoints.empty())
        return std::unexpected(ErrorMessage("Shader reflection is incomplete for a compiled entry point"));

    std::vector<ShaderBinding>           Bindings;
    std::vector<ShaderPushConstantRange> PushConstants;
    if (auto R = CollectProgramInterface(ProgramLayout, EntryPoints, Bindings, PushConstants); !R)
        return std::unexpected(std::move(R.error()));

    return ShaderReflection{
        .Bindings      = std::move(Bindings),
        .PushConstants = MergePushConstantRanges(std::move(PushConstants)),
        .VertexInputs  = {},
        .BindlessSpace = ExtractBindlessSpace(ProgramLayout),
    };
}

} // namespace SoulEngine
