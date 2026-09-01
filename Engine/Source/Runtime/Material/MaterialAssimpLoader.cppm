module;

#include <assimp/material.h>
#include <entt/entt.hpp>
#include <hlsl++.h>

export module Material:AssimpLoader;

import std;

export import Core;
import :Types;
import :Texture;

namespace SoulEngine {

namespace {

[[nodiscard]] auto ReadColor3(
    const aiMaterial& Source,
    const char* Key,
    Uint32 Type,
    Uint32 Index,
    hlslpp::float3 Fallback) -> hlslpp::float3 {
    aiColor3D Value = {};
    if (Source.Get(Key, Type, Index, Value) != AI_SUCCESS)
        return Fallback;
    return hlslpp::float3{Value.r, Value.g, Value.b};
}

[[nodiscard]] auto ReadColor4(
    const aiMaterial& Source,
    const char* Key,
    Uint32 Type,
    Uint32 Index,
    hlslpp::float4 Fallback) -> hlslpp::float4 {
    aiColor4D Value = {};
    if (Source.Get(Key, Type, Index, Value) != AI_SUCCESS)
        return Fallback;
    return hlslpp::float4{Value.r, Value.g, Value.b, Value.a};
}

[[nodiscard]] auto ReadFloat(
    const aiMaterial& Source,
    const char* Key,
    Uint32 Type,
    Uint32 Index,
    Float32 Fallback) -> Float32 {
    ai_real Value = static_cast<ai_real>(Fallback);
    return Source.Get(Key, Type, Index, Value) == AI_SUCCESS
               ? static_cast<Float32>(Value)
               : Fallback;
}

[[nodiscard]] auto ReadInt(
    const aiMaterial& Source,
    const char* Key,
    Uint32 Type,
    Uint32 Index,
    Int32 Fallback) -> Int32 {
    int Value = static_cast<int>(Fallback);
    return Source.Get(Key, Type, Index, Value) == AI_SUCCESS
               ? static_cast<Int32>(Value)
               : Fallback;
}

[[nodiscard]] auto ToTextureMapping(aiTextureMapping Value) -> TextureMapping {
    switch (Value) {
    case aiTextureMapping_UV:        return TextureMapping::UV;
    case aiTextureMapping_SPHERE:    return TextureMapping::Sphere;
    case aiTextureMapping_CYLINDER:  return TextureMapping::Cylinder;
    case aiTextureMapping_BOX:       return TextureMapping::Box;
    case aiTextureMapping_PLANE:     return TextureMapping::Plane;
    case aiTextureMapping_OTHER:     return TextureMapping::Other;
    default:                         return TextureMapping::Unknown;
    }
}

[[nodiscard]] auto ToTextureOperation(aiTextureOp Value) -> TextureOperation {
    switch (Value) {
    case aiTextureOp_Multiply:  return TextureOperation::Multiply;
    case aiTextureOp_Add:       return TextureOperation::Add;
    case aiTextureOp_Subtract:  return TextureOperation::Subtract;
    case aiTextureOp_Divide:    return TextureOperation::Divide;
    case aiTextureOp_SmoothAdd: return TextureOperation::SmoothAdd;
    case aiTextureOp_SignedAdd: return TextureOperation::SignedAdd;
    default:                    return TextureOperation::Unknown;
    }
}

[[nodiscard]] auto ToTextureMapMode(aiTextureMapMode Value) -> TextureMapMode {
    switch (Value) {
    case aiTextureMapMode_Wrap:   return TextureMapMode::Wrap;
    case aiTextureMapMode_Clamp:  return TextureMapMode::Clamp;
    case aiTextureMapMode_Mirror: return TextureMapMode::Mirror;
    case aiTextureMapMode_Decal:  return TextureMapMode::Decal;
    default:                      return TextureMapMode::Unknown;
    }
}

[[nodiscard]] auto ToBlendFunction(aiBlendMode Value) -> MaterialBlendFunction {
    switch (Value) {
    case aiBlendMode_Default:  return MaterialBlendFunction::Default;
    case aiBlendMode_Additive: return MaterialBlendFunction::Additive;
    default:                   return MaterialBlendFunction::Unknown;
    }
}

[[nodiscard]] auto ToShadingModel(aiShadingMode Value) -> MaterialShadingModel {
    switch (Value) {
    case aiShadingMode_Flat:          return MaterialShadingModel::Flat;
    case aiShadingMode_Gouraud:       return MaterialShadingModel::Gouraud;
    case aiShadingMode_Phong:         return MaterialShadingModel::Phong;
    case aiShadingMode_Blinn:         return MaterialShadingModel::Blinn;
    case aiShadingMode_Toon:          return MaterialShadingModel::Toon;
    case aiShadingMode_OrenNayar:     return MaterialShadingModel::OrenNayar;
    case aiShadingMode_Minnaert:      return MaterialShadingModel::Minnaert;
    case aiShadingMode_CookTorrance:  return MaterialShadingModel::CookTorrance;
    case aiShadingMode_NoShading:     return MaterialShadingModel::Unlit;
    case aiShadingMode_Fresnel:       return MaterialShadingModel::Fresnel;
    case aiShadingMode_PBR_BRDF:      return MaterialShadingModel::Pbr;
    default:                          return MaterialShadingModel::Unknown;
    }
}

[[nodiscard]] auto LoadTextureSlot(
    const aiMaterial& Source,
    aiTextureType Type,
    Uint32 Index,
    const Path& ModelDirectory,
    TextureDataCache& TextureCache) -> std::optional<TextureRecord> {
    aiString SourcePath = {};
    aiTextureMapping Mapping = aiTextureMapping_UV;
    Uint32 UVIndex = 0;
    ai_real Blend = 1.0f;
    aiTextureOp Operation = aiTextureOp_Multiply;
    std::array<aiTextureMapMode, 3> MapModes = {
        aiTextureMapMode_Wrap,
        aiTextureMapMode_Wrap,
        aiTextureMapMode_Wrap,
    };
    if (Source.GetTexture(Type, Index, &SourcePath, &Mapping, &UVIndex, &Blend, &Operation, MapModes.data()) != AI_SUCCESS)
        return std::nullopt;

    if (SourcePath.length == 0 || SourcePath.C_Str()[0] == '*') {
        LogWarning("MaterialAssimpLoader: embedded texture '{}' is unsupported", SourcePath.C_Str());
        return std::nullopt;
    }

    const Path RelativePath = SourcePath.C_Str();
    if (RelativePath.is_absolute()) {
        LogWarning("MaterialAssimpLoader: absolute texture path '{}' is unsupported", SourcePath.C_Str());
        return std::nullopt;
    }

    const Path AssetPath = (ModelDirectory / RelativePath).lexically_normal();
    const auto AssetPathString = AssetPath.string();
    const auto TextureId = entt::hashed_string{AssetPathString.data(), AssetPathString.size()};
    auto [It, Loaded] = TextureCache.load(TextureId, AssetPath);
    if (!It->second) {
        LogWarning("MaterialAssimpLoader: failed to load texture '{}'", AssetPathString);
        return std::nullopt;
    }
    const auto ReadInteger = [&Source, Type, Index](const char* Key, Uint32 Fallback) {
        return static_cast<Uint32>(ReadInt(Source, Key, Type, Index, static_cast<Int32>(Fallback)));
    };
    return TextureRecord{
        .Texture   = It->second,
        .Mapping   = ToTextureMapping(Mapping),
        .UVIndex   = UVIndex,
        .Blend     = static_cast<Float32>(Blend),
        .Operation = ToTextureOperation(Operation),
        .MapModeU  = ToTextureMapMode(MapModes[0]),
        .MapModeV  = ToTextureMapMode(MapModes[1]),
        .MapModeW  = ToTextureMapMode(MapModes[2]),
        .Flags     = ReadInteger(_AI_MATKEY_TEXFLAGS_BASE, 0),
    };
}

} // namespace

} // namespace SoulEngine

export namespace SoulEngine {

/// @brief Loads one Assimp material into a renderer-neutral material record.
struct MaterialAssimpLoader {
    using result_type = SPtr<MaterialRecord>;

    MaterialAssimpLoader() = default;

    auto operator()(
        const aiMaterial* Source,
        const Path& ModelDirectory) -> result_type {
        if (!Source) {
            LogWarning("MaterialAssimpLoader: material source is null");
            return nullptr;
        }

        aiString MaterialName = {};
        const bool HasName =
            Source->Get(AI_MATKEY_NAME, MaterialName) == AI_SUCCESS && MaterialName.length != 0;

        auto Result = std::make_shared<MaterialRecord>();
        Result->Name = HasName ? String(MaterialName.C_Str()) : String{"Material"};
        Result->Ambient = ReadColor3(
            *Source, AI_MATKEY_COLOR_AMBIENT, Result->Ambient);
        Result->Diffuse = ReadColor3(
            *Source, AI_MATKEY_COLOR_DIFFUSE, Result->Diffuse);
        Result->Specular = ReadColor3(
            *Source, AI_MATKEY_COLOR_SPECULAR, Result->Specular);
        Result->Emissive = ReadColor3(
            *Source, AI_MATKEY_COLOR_EMISSIVE, Result->Emissive);
        Result->Transparent = ReadColor3(
            *Source, AI_MATKEY_COLOR_TRANSPARENT, Result->Transparent);
        Result->Reflective = ReadColor3(
            *Source, AI_MATKEY_COLOR_REFLECTIVE, Result->Reflective);

        Result->Opacity = ReadFloat(*Source, AI_MATKEY_OPACITY, Result->Opacity);
        Result->BumpScaling = ReadFloat(*Source, AI_MATKEY_BUMPSCALING, Result->BumpScaling);
        Result->Shininess = ReadFloat(*Source, AI_MATKEY_SHININESS, Result->Shininess);
        Result->ShininessStrength =
            ReadFloat(*Source, AI_MATKEY_SHININESS_STRENGTH, Result->ShininessStrength);
        Result->RefractionIndex = ReadFloat(*Source, AI_MATKEY_REFRACTI, Result->RefractionIndex);
        Result->Reflectivity = ReadFloat(*Source, AI_MATKEY_REFLECTIVITY, Result->Reflectivity);

        Result->BaseColorFactor = ReadColor4(
            *Source, AI_MATKEY_BASE_COLOR, Result->BaseColorFactor);

        Result->MetallicFactor = std::clamp(
            ReadFloat(*Source, AI_MATKEY_METALLIC_FACTOR, Result->MetallicFactor), 0.0f, 1.0f);
        Result->RoughnessFactor = std::clamp(
            ReadFloat(*Source, AI_MATKEY_ROUGHNESS_FACTOR, Result->RoughnessFactor), 0.0f, 1.0f);
        Result->SheenColorFactor = ReadColor3(
            *Source, AI_MATKEY_SHEEN_COLOR_FACTOR, Result->SheenColorFactor);
        Result->SheenRoughnessFactor = ReadFloat(
            *Source, AI_MATKEY_SHEEN_ROUGHNESS_FACTOR, Result->SheenRoughnessFactor);
        Result->ClearcoatFactor = ReadFloat(
            *Source, AI_MATKEY_CLEARCOAT_FACTOR, Result->ClearcoatFactor);
        Result->ClearcoatRoughnessFactor = ReadFloat(
            *Source, AI_MATKEY_CLEARCOAT_ROUGHNESS_FACTOR, Result->ClearcoatRoughnessFactor);
        Result->TransmissionFactor = ReadFloat(
            *Source, AI_MATKEY_TRANSMISSION_FACTOR, Result->TransmissionFactor);
        Result->VolumeThicknessFactor = ReadFloat(
            *Source, AI_MATKEY_VOLUME_THICKNESS_FACTOR, Result->VolumeThicknessFactor);
        Result->VolumeAttenuationDistance = ReadFloat(
            *Source, AI_MATKEY_VOLUME_ATTENUATION_DISTANCE, Result->VolumeAttenuationDistance);
        Result->VolumeAttenuationColor = ReadColor3(
            *Source, AI_MATKEY_VOLUME_ATTENUATION_COLOR, Result->VolumeAttenuationColor);

        const Int32 EnableWireframe = ReadInt(*Source, AI_MATKEY_ENABLE_WIREFRAME, 0);
        Result->EnableWireframe = EnableWireframe != 0;
        const Int32 TwoSided = ReadInt(*Source, AI_MATKEY_TWOSIDED, 0);
        Result->TwoSided = TwoSided != 0;
        const Int32 BlendFunction = ReadInt(*Source, AI_MATKEY_BLEND_FUNC, 0);
        Result->BlendFunction = ToBlendFunction(static_cast<aiBlendMode>(BlendFunction));
        const Int32 ShadingModel = ReadInt(*Source, AI_MATKEY_SHADING_MODEL, 0);
        Result->ShadingModel = ToShadingModel(static_cast<aiShadingMode>(ShadingModel));

        for (Uint32 TypeValue = aiTextureType_NONE;
             TypeValue <= AI_TEXTURE_TYPE_MAX;
             ++TypeValue) {
            const auto Type = static_cast<aiTextureType>(TypeValue);
            auto& TextureSlots = Result->Textures[TypeValue];
            TextureSlots.reserve(Source->GetTextureCount(Type));
            for (Uint32 Index = 0; Index < Source->GetTextureCount(Type); ++Index) {
                if (auto Slot = LoadTextureSlot(
                        *Source, Type, Index, ModelDirectory, m_TextureCache)) {
                    TextureSlots.emplace_back(std::move(*Slot));
                }
            }
        }

        return Result;
    }

  private:
    TextureDataCache m_TextureCache = {};
};

using MaterialHandle = entt::resource<MaterialRecord>;
using MaterialAssimpCache = entt::resource_cache<MaterialRecord, MaterialAssimpLoader>;

[[nodiscard]] auto MakeMaterialCacheKey(
    const Path& ModelPath,
    StringView MaterialName,
    Uint32 MaterialIndex) -> entt::id_type {
    const auto KeyText =
        Format("{}#{}#{}", ModelPath.lexically_normal().string(), MaterialName, MaterialIndex);
    return entt::hashed_string{KeyText.data(), KeyText.size()};
}

} // namespace SoulEngine
