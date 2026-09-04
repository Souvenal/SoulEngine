/// @file   SlangCompiler/SlangTypes.cppm
/// @brief  Slang-to-engine type mapping utilities.
///
/// Pure conversion functions: Slang SDK enums / reflection types ->
/// engine-side Shader module types.  No state, no I/O, no config access.

module;

#include <slang.h>

export module Slang:Types;

import magic_enum;
import std;
import Core;
import Shader;

namespace SoulEngine {

// File-local helpers for normalizing Slang reflection details before public mappings.
namespace {

[[nodiscard]] auto ToShaderTextureResourceType(SlangResourceAccess Access)
    -> std::expected<ShaderResourceType, ErrorMessage> {
    switch (Access) {
    case SLANG_RESOURCE_ACCESS_NONE:
    case SLANG_RESOURCE_ACCESS_READ:
        return ShaderResourceType::SampledTexture;
    case SLANG_RESOURCE_ACCESS_READ_WRITE:
    case SLANG_RESOURCE_ACCESS_WRITE:
    case SLANG_RESOURCE_ACCESS_RASTER_ORDERED:
    case SLANG_RESOURCE_ACCESS_APPEND:
    case SLANG_RESOURCE_ACCESS_CONSUME:
    case SLANG_RESOURCE_ACCESS_FEEDBACK:
        return ShaderResourceType::StorageTexture;
    default:
        return std::unexpected(ErrorMessage("Unsupported texture access mode in reflection"));
    }
}

} // namespace

[[nodiscard]] auto StripArrayTypeLayout(slang::TypeLayoutReflection* TypeLayout) -> slang::TypeLayoutReflection* {
    while (TypeLayout && TypeLayout->getKind() == slang::TypeReflection::Kind::Array)
        TypeLayout = TypeLayout->getElementTypeLayout();
    return TypeLayout;
}

/// Map the type layout of a leaf variable (array layers stripped) to the
/// engine's ResourceType.  Used by the reflection DFS, which reaches leaves
/// through the variable tree rather than through binding ranges.
[[nodiscard]] auto ToShaderResourceTypeFromLeafLayout(slang::TypeLayoutReflection* TypeLayout)
    -> std::expected<ShaderResourceType, ErrorMessage> {
    TypeLayout = StripArrayTypeLayout(TypeLayout);
    if (!TypeLayout)
        return std::unexpected(ErrorMessage("Binding reflection is missing a type layout"));

    switch (TypeLayout->getKind()) {
    case slang::TypeReflection::Kind::ConstantBuffer:
        return ShaderResourceType::ConstantBuffer;
    case slang::TypeReflection::Kind::SamplerState:
        return ShaderResourceType::Sampler;
    case slang::TypeReflection::Kind::Resource: {
        const auto BaseResourceShape = static_cast<SlangResourceShapeIntegral>(TypeLayout->getResourceShape()) &
            static_cast<SlangResourceShapeIntegral>(SLANG_RESOURCE_BASE_SHAPE_MASK);
        if (BaseResourceShape == static_cast<SlangResourceShapeIntegral>(SLANG_ACCELERATION_STRUCTURE))
            return ShaderResourceType::AccelerationStructure;
        if (BaseResourceShape == static_cast<SlangResourceShapeIntegral>(SLANG_STRUCTURED_BUFFER) ||
            BaseResourceShape == static_cast<SlangResourceShapeIntegral>(SLANG_BYTE_ADDRESS_BUFFER)) {
            return ShaderResourceType::StorageBuffer;
        }
        return ToShaderTextureResourceType(TypeLayout->getResourceAccess());
    }
    default:
        return std::unexpected(ErrorMessage(Format("Unsupported leaf type kind {} in reflection",
                                                   magic_enum::enum_name(TypeLayout->getKind()))));
    }
}

/// Map a SlangStage enum value to the project's Stage.
[[nodiscard]] auto ToShaderStage(SlangStage Stage) -> ShaderStage {
    switch (Stage) {
    case SLANG_STAGE_VERTEX:
        return ShaderStage::Vertex;
    case SLANG_STAGE_FRAGMENT:
        return ShaderStage::Fragment;
    case SLANG_STAGE_COMPUTE:
        return ShaderStage::Compute;
    case SLANG_STAGE_HULL:
        return ShaderStage::Hull;
    case SLANG_STAGE_DOMAIN:
        return ShaderStage::Domain;
    case SLANG_STAGE_GEOMETRY:
        return ShaderStage::Geometry;
    case SLANG_STAGE_MESH:
        return ShaderStage::Mesh;
    case SLANG_STAGE_AMPLIFICATION:
        return ShaderStage::Amplification;
    case SLANG_STAGE_RAY_GENERATION:
        return ShaderStage::RayGeneration;
    case SLANG_STAGE_INTERSECTION:
        return ShaderStage::Intersection;
    case SLANG_STAGE_ANY_HIT:
        return ShaderStage::AnyHit;
    case SLANG_STAGE_CLOSEST_HIT:
        return ShaderStage::ClosestHit;
    case SLANG_STAGE_MISS:
        return ShaderStage::Miss;
    case SLANG_STAGE_CALLABLE:
        return ShaderStage::Callable;
    default:
        return ShaderStage::Unknown;
    }
}

[[nodiscard]] auto ToShaderScalarType(slang::TypeReflection::ScalarType ScalarType) -> ShaderScalarType {
    switch (ScalarType) {
    case slang::TypeReflection::ScalarType::Float32:
        return ShaderScalarType::Float32;
    case slang::TypeReflection::ScalarType::Int32:
        return ShaderScalarType::Int32;
    case slang::TypeReflection::ScalarType::UInt32:
        return ShaderScalarType::Uint32;
    default:
        return ShaderScalarType::Unknown;
    }
}

/// Build a ValueType from a Slang type-layout reflection object.
[[nodiscard]] auto ToShaderValueType(slang::TypeLayoutReflection* TypeLayout) -> ShaderValueType {
    if (!TypeLayout)
        return {};

    return ShaderValueType{
        .ScalarType  = ToShaderScalarType(TypeLayout->getScalarType()),
        .RowCount    = TypeLayout->getRowCount(),
        .ColumnCount = TypeLayout->getColumnCount(),
    };
}

} // namespace SoulEngine
