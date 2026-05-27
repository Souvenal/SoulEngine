/// @file   SlangCompiler/SlangTypes.cppm
/// @brief  Slang-to-engine type mapping utilities.
///
/// Pure conversion functions: Slang SDK enums / reflection types ->
/// engine-side Shader module types.  No state, no I/O, no config access.

module;

#include <slang.h>

export module Slang:Types;

import Core;
import Shader;

namespace SoulEngine::ShaderCompiler::SlangCompiler {

using namespace SoulEngine::Core;

/// Map a SlangStage enum value to the project's Stage.
[[nodiscard]] auto ToShaderStage(SlangStage Stage) -> Shader::Stage {
    switch (Stage) {
    case SLANG_STAGE_VERTEX:
        return Shader::Stage::Vertex;
    case SLANG_STAGE_FRAGMENT:
        return Shader::Stage::Fragment;
    case SLANG_STAGE_COMPUTE:
        return Shader::Stage::Compute;
    case SLANG_STAGE_HULL:
        return Shader::Stage::Hull;
    case SLANG_STAGE_DOMAIN:
        return Shader::Stage::Domain;
    case SLANG_STAGE_GEOMETRY:
        return Shader::Stage::Geometry;
    case SLANG_STAGE_MESH:
        return Shader::Stage::Mesh;
    case SLANG_STAGE_AMPLIFICATION:
        return Shader::Stage::Amplification;
    default:
        return Shader::Stage::Unknown;
    }
}

/// Map a Slang binding type + optional type layout to the engine's ResourceType.
[[nodiscard]] auto ToShaderResourceType(slang::BindingType BindingType, slang::TypeLayoutReflection* TypeLayout)
    -> std::expected<Shader::ResourceType, ErrorMessage> {
    switch (BindingType) {
    case slang::BindingType::ConstantBuffer:
        return Shader::ResourceType::UniformBuffer;
    case slang::BindingType::Sampler:
        return Shader::ResourceType::Sampler;
    case slang::BindingType::Texture:
        if (!TypeLayout)
            return std::unexpected(ErrorMessage("Texture binding reflection is missing a type layout"));
        switch (TypeLayout->getResourceAccess()) {
        case SLANG_RESOURCE_ACCESS_NONE:
        case SLANG_RESOURCE_ACCESS_READ:
            return Shader::ResourceType::SampledTexture;
        case SLANG_RESOURCE_ACCESS_READ_WRITE:
        case SLANG_RESOURCE_ACCESS_WRITE:
        case SLANG_RESOURCE_ACCESS_RASTER_ORDERED:
        case SLANG_RESOURCE_ACCESS_APPEND:
        case SLANG_RESOURCE_ACCESS_CONSUME:
        case SLANG_RESOURCE_ACCESS_FEEDBACK:
            return Shader::ResourceType::StorageTexture;
        default:
            return std::unexpected(ErrorMessage("Unsupported texture access mode in reflection"));
        }
    case slang::BindingType::TypedBuffer:
    case slang::BindingType::RawBuffer:
        return Shader::ResourceType::StorageBuffer;
    default:
        return std::unexpected(ErrorMessage(
            Format("Unsupported Slang binding type {} in normalized reflection", static_cast<int>(BindingType))));
    }
}
[[nodiscard]] auto ToShaderScalarType(slang::TypeReflection::ScalarType ScalarType) -> Shader::ScalarType {
    switch (ScalarType) {
    case slang::TypeReflection::ScalarType::Float32:
        return Shader::ScalarType::Float32;
    case slang::TypeReflection::ScalarType::Int32:
        return Shader::ScalarType::Int32;
    case slang::TypeReflection::ScalarType::UInt32:
        return Shader::ScalarType::Uint32;
    default:
        return Shader::ScalarType::Unknown;
    }
}

/// Build a ValueType from a Slang type-layout reflection object.
[[nodiscard]] auto ToShaderValueType(slang::TypeLayoutReflection* TypeLayout) -> Shader::ValueType {
    if (!TypeLayout)
        return {};

    return Shader::ValueType{
        .ScalarType  = ToShaderScalarType(TypeLayout->getScalarType()),
        .RowCount    = TypeLayout->getRowCount(),
        .ColumnCount = TypeLayout->getColumnCount(),
    };
}

} // namespace SoulEngine::ShaderCompiler::SlangCompiler
