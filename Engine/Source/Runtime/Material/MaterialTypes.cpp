module;

#include <hlsl++.h>

// magic_enum::case_insensitive is not exported in v0.9.8. master branch has a fix for this, however it can't
// pass compilation with c++23. Before it is fixed, we now use header only in this file.
#include <magic_enum/magic_enum.hpp>

module Material:Types.impl;

import :Types;
import Core;
import RHI;
import std;

namespace SoulEngine {

auto MaterialYamlRecord::SetBlendFunction(String Value) -> void {
    if (const auto Parsed = magic_enum::enum_cast<MaterialBlendFunction>(Value, magic_enum::case_insensitive)) {
        BlendFunction = *Parsed;
        return;
    }
    LogWarning("MaterialYamlRecord: unknown blend function '{}'; keeping current value", Value);
}

[[nodiscard]] auto MaterialYamlRecord::GetBlendFunction() const -> String {
    return String(magic_enum::enum_name(BlendFunction));
}

auto MaterialYamlRecord::SetAlphaMode(String Value) -> void {
    if (const auto Parsed = magic_enum::enum_cast<MaterialAlphaMode>(Value, magic_enum::case_insensitive)) {
        AlphaMode = *Parsed;
        return;
    }
    LogWarning("MaterialYamlRecord: unknown alpha mode '{}'; keeping current value", Value);
}

[[nodiscard]] auto MaterialYamlRecord::GetAlphaMode() const -> String {
    return String(magic_enum::enum_name(AlphaMode));
}

auto MaterialYamlRecord::SetShadingModel(String Value) -> void {
    if (const auto Parsed = magic_enum::enum_cast<MaterialShadingModel>(Value, magic_enum::case_insensitive)) {
        ShadingModel = *Parsed;
        return;
    }
    LogWarning("MaterialYamlRecord: unknown shading model '{}'; keeping current value", Value);
}

[[nodiscard]] auto MaterialYamlRecord::GetShadingModel() const -> String {
    return String(magic_enum::enum_name(ShadingModel));
}

[[nodiscard]] auto
MaterialRecord::HasUnresolvedBindlessTextures(const std::unordered_map<Uint64, Uint32>& TextureIndices) const -> bool {
    const auto FirstSlot = [this](TextureType Type) -> TextureDataHandle {
        const auto Slot =
            std::ranges::find_if(Textures, [Type](const TextureRecord& Record) -> bool { return Record.Type == Type; });
        return Slot == Textures.end() ? TextureDataHandle{} : Slot->Texture;
    };
    const auto IsUnresolved = [&TextureIndices](const TextureDataHandle& Texture) -> bool {
        return Texture && Texture->Texture && !TextureIndices.contains(Texture->Texture->GetId());
    };

    auto BaseColor = FirstSlot(TextureType::BaseColor);
    if (!BaseColor)
        BaseColor = FirstSlot(TextureType::Diffuse);
    auto Normal = FirstSlot(TextureType::NormalCamera);
    if (!Normal)
        Normal = FirstSlot(TextureType::Normals);
    auto Emissive = FirstSlot(TextureType::EmissionColor);
    if (!Emissive)
        Emissive = FirstSlot(TextureType::Emissive);
    if (IsUnresolved(BaseColor) || IsUnresolved(Normal) || IsUnresolved(Emissive) ||
        IsUnresolved(FirstSlot(TextureType::AmbientOcclusion)))
        return true;

    const auto GltfMetallicRoughness = FirstSlot(TextureType::GltfMetallicRoughness);
    if (GltfMetallicRoughness)
        return IsUnresolved(GltfMetallicRoughness);

    const auto Metallic  = FirstSlot(TextureType::Metalness);
    const auto Roughness = FirstSlot(TextureType::DiffuseRoughness);
    if (Metallic && Roughness && Metallic->Texture == Roughness->Texture)
        return IsUnresolved(Metallic);
    return IsUnresolved(Metallic) || IsUnresolved(Roughness);
}

[[nodiscard]] auto MaterialRecord::BuildGpuData(const std::unordered_map<Uint64, Uint32>& TextureIndices) const
    -> GpuData {
    const auto FirstSlot = [this](TextureType Type) -> TextureDataHandle {
        const auto Slot =
            std::ranges::find_if(Textures, [Type](const TextureRecord& Record) -> bool { return Record.Type == Type; });
        return Slot == Textures.end() ? TextureDataHandle{} : Slot->Texture;
    };
    const auto ResolveTexture = [&TextureIndices](const TextureDataHandle& Texture) -> std::array<Uint32, 2> {
        if (!Texture || !Texture->Texture)
            return {};
        const auto Index = TextureIndices.find(Texture->Texture->GetId());
        return Index != TextureIndices.end() ? std::array<Uint32, 2>{Index->second, 0} : std::array<Uint32, 2>{};
    };

    GpuData Result{
        .Ambient                   = hlslpp::interop::float3{Ambient},
        .Diffuse                   = hlslpp::interop::float3{Diffuse},
        .Specular                  = hlslpp::interop::float3{Specular},
        .Emissive                  = hlslpp::interop::float3{Emissive},
        .Transparent               = hlslpp::interop::float3{Transparent},
        .Reflective                = hlslpp::interop::float3{Reflective},
        .Opacity                   = Opacity,
        .BumpScaling               = BumpScaling,
        .Shininess                 = Shininess,
        .ShininessStrength         = ShininessStrength,
        .RefractionIndex           = RefractionIndex,
        .Reflectivity              = Reflectivity,
        .EnableWireframe           = EnableWireframe ? 1U : 0U,
        .TwoSided                  = TwoSided ? 1U : 0U,
        .BlendFunction             = static_cast<Uint32>(BlendFunction),
        .ShadingModel              = static_cast<Uint32>(ShadingModel),
        .AlphaMode                 = static_cast<Uint32>(AlphaMode),
        .AlphaCutoff               = AlphaCutoff,
        .BaseColorFactor           = hlslpp::interop::float4{BaseColorFactor},
        .MetallicFactor            = MetallicFactor,
        .RoughnessFactor           = RoughnessFactor,
        .SheenColorFactor          = hlslpp::interop::float3{SheenColorFactor},
        .SheenRoughnessFactor      = SheenRoughnessFactor,
        .ClearcoatFactor           = ClearcoatFactor,
        .ClearcoatRoughnessFactor  = ClearcoatRoughnessFactor,
        .TransmissionFactor        = TransmissionFactor,
        .VolumeThicknessFactor     = VolumeThicknessFactor,
        .VolumeAttenuationDistance = VolumeAttenuationDistance,
        .VolumeAttenuationColor    = hlslpp::interop::float3{VolumeAttenuationColor},
    };

    auto BaseColor = FirstSlot(TextureType::BaseColor);
    if (!BaseColor)
        BaseColor = FirstSlot(TextureType::Diffuse);
    auto Normal = FirstSlot(TextureType::NormalCamera);
    if (!Normal)
        Normal = FirstSlot(TextureType::Normals);
    auto EmissiveSlot = FirstSlot(TextureType::EmissionColor);
    if (!EmissiveSlot)
        EmissiveSlot = FirstSlot(TextureType::Emissive);

    const auto GltfMetallicRoughness = FirstSlot(TextureType::GltfMetallicRoughness);

    Result.BaseColorTexture = ResolveTexture(BaseColor);
    Result.NormalTexture    = ResolveTexture(Normal);
    if (GltfMetallicRoughness) {
        Result.MetallicRoughnessTexture = ResolveTexture(GltfMetallicRoughness);
    } else {
        const auto Metallic                = FirstSlot(TextureType::Metalness);
        const auto Roughness               = FirstSlot(TextureType::DiffuseRoughness);
        const bool SharedMetallicRoughness = Metallic && Roughness && Metallic->Texture == Roughness->Texture;
        if (SharedMetallicRoughness) {
            Result.MetallicRoughnessTexture = ResolveTexture(Metallic);
        } else {
            Result.MetallicTexture  = ResolveTexture(Metallic);
            Result.RoughnessTexture = ResolveTexture(Roughness);
        }
    }
    Result.OcclusionTexture = ResolveTexture(FirstSlot(TextureType::AmbientOcclusion));
    Result.EmissiveTexture  = ResolveTexture(EmissiveSlot);
    return Result;
}

} // namespace SoulEngine
