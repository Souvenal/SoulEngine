module;

#include <entt/entt.hpp>

export module Material:YamlLoader;

import std;
import Core;
import :Types;
import :Texture;

namespace SoulEngine {
namespace {

[[nodiscard]] auto IsRelativeAssetPath(StringView Value) -> bool {
    const Path Asset{String(Value)};
    if (Value.empty() || Asset.is_absolute() || Asset == Path{"."} || Asset.lexically_normal() != Asset)
        return false;
    for (const auto& Part : Asset) {
        if (Part == "..")
            return false;
    }
    return true;
}

auto RegisterMaterialYamlMeta() -> void {
    entt::meta_factory<MaterialYamlRecord> Factory = entt::meta_factory<MaterialYamlRecord>{}.type("material");
    Factory.data<&MaterialYamlRecord::Name>("name");
    Factory.data<&MaterialYamlRecord::Ambient>("ambient");
    Factory.data<&MaterialYamlRecord::Diffuse>("diffuse");
    Factory.data<&MaterialYamlRecord::Specular>("specular");
    Factory.data<&MaterialYamlRecord::Emissive>("emissive");
    Factory.data<&MaterialYamlRecord::Transparent>("transparent");
    Factory.data<&MaterialYamlRecord::Reflective>("reflective");
    Factory.data<&MaterialYamlRecord::Opacity>("opacity");
    Factory.data<&MaterialYamlRecord::BumpScaling>("bump_scaling");
    Factory.data<&MaterialYamlRecord::Shininess>("shininess");
    Factory.data<&MaterialYamlRecord::ShininessStrength>("shininess_strength");
    Factory.data<&MaterialYamlRecord::RefractionIndex>("refraction_index");
    Factory.data<&MaterialYamlRecord::Reflectivity>("reflectivity");
    Factory.data<&MaterialYamlRecord::EnableWireframe>("enable_wireframe");
    Factory.data<&MaterialYamlRecord::TwoSided>("two_sided");
    Factory.data<&MaterialYamlRecord::SetBlendFunction, &MaterialYamlRecord::GetBlendFunction>("blend_function");
    Factory.data<&MaterialYamlRecord::SetShadingModel, &MaterialYamlRecord::GetShadingModel>("shading_model");
    Factory.data<&MaterialYamlRecord::SetAlphaMode, &MaterialYamlRecord::GetAlphaMode>("alpha_mode");
    Factory.data<&MaterialYamlRecord::AlphaCutoff>("alpha_cutoff");
    Factory.data<&MaterialYamlRecord::BaseColorFactor>("base_color_factor");
    Factory.data<&MaterialYamlRecord::MetallicFactor>("metallic_factor");
    Factory.data<&MaterialYamlRecord::RoughnessFactor>("roughness_factor");
    Factory.data<&MaterialYamlRecord::SheenColorFactor>("sheen_color_factor");
    Factory.data<&MaterialYamlRecord::SheenRoughnessFactor>("sheen_roughness_factor");
    Factory.data<&MaterialYamlRecord::ClearcoatFactor>("clearcoat_factor");
    Factory.data<&MaterialYamlRecord::ClearcoatRoughnessFactor>("clearcoat_roughness_factor");
    Factory.data<&MaterialYamlRecord::TransmissionFactor>("transmission_factor");
    Factory.data<&MaterialYamlRecord::VolumeThicknessFactor>("volume_thickness_factor");
    Factory.data<&MaterialYamlRecord::VolumeAttenuationDistance>("volume_attenuation_distance");
    Factory.data<&MaterialYamlRecord::VolumeAttenuationColor>("volume_attenuation_color");
    Factory.data<&MaterialYamlRecord::BaseColorTexture>("base_color_texture");
    Factory.data<&MaterialYamlRecord::NormalTexture>("normal_texture");
    Factory.data<&MaterialYamlRecord::MetallicRoughnessTexture>("metallic_roughness_texture");
    Factory.data<&MaterialYamlRecord::MetallicTexture>("metallic_texture");
    Factory.data<&MaterialYamlRecord::RoughnessTexture>("roughness_texture");
    Factory.data<&MaterialYamlRecord::OcclusionTexture>("occlusion_texture");
    Factory.data<&MaterialYamlRecord::EmissiveTexture>("emissive_texture");
}

} // namespace

// Note: this namespace must stay named. In a named module, clang 23 silently
// drops the dynamic initializer of an unreferenced anonymous-namespace variable,
// which would skip this entt meta registration at program startup.
namespace MetaRegistration {

struct MaterialYamlMetaRegistration { MaterialYamlMetaRegistration() { RegisterMaterialYamlMeta(); } };
MaterialYamlMetaRegistration g_MaterialYamlMetaRegistration = {};

} // namespace MetaRegistration
} // namespace SoulEngine

export namespace SoulEngine {

/// @brief Loads one complete Material YAML file into a runtime material record.
struct MaterialYamlLoader {
    using result_type = SPtr<MaterialRecord>;

    auto operator()(const Path& MaterialPath, const Path& AssetRoot) -> result_type {
        auto Document = YamlDocument::Create(MaterialPath);
        if (!Document) {
            LogWarning("MaterialYamlLoader: {}", Document.error().ToString());
            return nullptr;
        }
        const auto& Root     = Document->Root();
        const auto  Material = Root["material"];
        if (!Material || !Material->get().IsMap()) {
            LogWarning("MaterialYamlLoader: '{}' has no material mapping", MaterialPath.string());
            return nullptr;
        }

        MaterialYamlRecord Value    = {};
        auto               Record   = entt::forward_as_meta(Value);
        const auto         MetaType = entt::resolve<MaterialYamlRecord>();
        for (const auto& [FieldName, FieldNodePtr] : Material->get().Items()) {
            const auto& FieldNode = *FieldNodePtr;
            const auto  Field     = MetaType.data(entt::hashed_string{FieldName.c_str(), FieldName.size()}.value());
            if (!Field) {
                LogWarning("MaterialYamlLoader: unknown field '{}' at {} in '{}'",
                           FieldName, FieldNode.Location(), MaterialPath.string());
                return nullptr;
            }
            // Invalid values keep the record's declared default for that field.
            if (auto Result = AssignYamlValue(Field, Record, FieldNode); !Result)
                LogWarning("MaterialYamlLoader: field '{}' at {} in '{}': {}",
                           FieldName, FieldNode.Location(), MaterialPath.string(), Result.error().ToString());
        }

        // Texture paths must stay relative to the Assets root before resolving.
        const auto ResolveTexture = [&AssetRoot, &MaterialPath](String& Text) -> void {
            if (Text.empty())
                return;
            if (!IsRelativeAssetPath(Text)) {
                LogWarning("MaterialYamlLoader: texture '{}' must be relative to Assets in '{}'",
                           Text, MaterialPath.string());
                Text.clear();
                return;
            }
            Text = (AssetRoot / Path(Text)).lexically_normal().string();
        };
        ResolveTexture(Value.BaseColorTexture);
        ResolveTexture(Value.NormalTexture);
        ResolveTexture(Value.MetallicRoughnessTexture);
        ResolveTexture(Value.MetallicTexture);
        ResolveTexture(Value.RoughnessTexture);
        ResolveTexture(Value.OcclusionTexture);
        ResolveTexture(Value.EmissiveTexture);

        auto Result = std::make_shared<MaterialRecord>();
        Result->Source = MaterialSource::Yaml;
        Result->SourceAsset = MaterialPath.lexically_normal();
        Result->Name = Value.Name.empty() ? String{"<unnamed>"} : Value.Name;
        Result->Ambient = Value.Ambient;
        Result->Diffuse = Value.Diffuse;
        Result->Specular = Value.Specular;
        Result->Emissive = Value.Emissive;
        Result->Transparent = Value.Transparent;
        Result->Reflective = Value.Reflective;
        Result->BaseColorFactor = Value.BaseColorFactor;
        Result->MetallicFactor = Value.MetallicFactor;
        Result->RoughnessFactor = Value.RoughnessFactor;
        Result->SheenColorFactor = Value.SheenColorFactor;
        Result->SheenRoughnessFactor = Value.SheenRoughnessFactor;
        Result->ClearcoatFactor = Value.ClearcoatFactor;
        Result->ClearcoatRoughnessFactor = Value.ClearcoatRoughnessFactor;
        Result->TransmissionFactor = Value.TransmissionFactor;
        Result->VolumeThicknessFactor = Value.VolumeThicknessFactor;
        Result->VolumeAttenuationDistance = Value.VolumeAttenuationDistance;
        Result->VolumeAttenuationColor = Value.VolumeAttenuationColor;
        Result->Opacity = Value.Opacity;
        Result->BumpScaling = Value.BumpScaling;
        Result->Shininess = Value.Shininess;
        Result->ShininessStrength = Value.ShininessStrength;
        Result->RefractionIndex = Value.RefractionIndex;
        Result->Reflectivity = Value.Reflectivity;
        Result->EnableWireframe = Value.EnableWireframe;
        Result->TwoSided = Value.TwoSided;
        Result->BlendFunction = Value.BlendFunction;
        Result->ShadingModel = Value.ShadingModel;
        Result->AlphaMode = Value.AlphaMode;
        Result->AlphaCutoff = Value.AlphaCutoff;

        LoadTexture(*Result, TextureType::BaseColor, Value.BaseColorTexture);
        LoadTexture(*Result, TextureType::Normals, Value.NormalTexture);
        if (!Value.MetallicRoughnessTexture.empty()) {
            LoadTexture(*Result, TextureType::Metalness, Value.MetallicRoughnessTexture);
            LoadTexture(*Result, TextureType::DiffuseRoughness, Value.MetallicRoughnessTexture);
        } else {
            LoadTexture(*Result, TextureType::Metalness, Value.MetallicTexture);
            LoadTexture(*Result, TextureType::DiffuseRoughness, Value.RoughnessTexture);
        }
        LoadTexture(*Result, TextureType::AmbientOcclusion, Value.OcclusionTexture);
        LoadTexture(*Result, TextureType::Emissive, Value.EmissiveTexture);
        return Result;
    }

  private:
    // TODO: Share this cache with MaterialAssimpLoader once material loading performance matters.
    TextureDataCache m_TextureCache = {};
    auto LoadTexture(MaterialRecord& Material, TextureType Type, const String& Path) -> void {
        if (Path.empty()) return;
        const auto Id = entt::hashed_string{Path.data(), Path.size()};
        auto [It, Loaded] = m_TextureCache.load(Id, SoulEngine::Path(Path));
        if (It->second) Material.Textures.emplace_back(TextureRecord{.Type = Type, .Texture = It->second});
        else LogWarning("MaterialYamlLoader: failed to load texture '{}'", Path);
    }
};

using MaterialYamlCache = entt::resource_cache<MaterialRecord, MaterialYamlLoader>;

} // namespace SoulEngine
