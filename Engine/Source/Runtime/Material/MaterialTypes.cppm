module;

#include <cstddef>
#include <entt/entt.hpp>
#include <hlsl++.h>

// magic_enum::case_insensitive is not exported in v0.9.8.
// master branch has a fix for this, however it can't pass compilation
// with c++23. Before it is fixed, we now use header only in this file.
#include <magic_enum/magic_enum.hpp>

export module Material:Types;

export import Core;
export import RHI;
export import std;

// import magic_enum;

export namespace SoulEngine {

enum class TextureMapping : Uint8 {
    Unknown = 0,
    UV,
    Sphere,
    Cylinder,
    Box,
    Plane,
    Other,
};

enum class TextureOperation : Uint8 {
    Unknown = 0,
    Multiply,
    Add,
    Subtract,
    Divide,
    SmoothAdd,
    SignedAdd,
};

enum class TextureMapMode : Uint8 {
    Unknown = 0,
    Wrap,
    Clamp,
    Mirror,
    Decal,
};

enum class TextureType : Uint8 {
    Unknown = 0,
    Diffuse,
    Specular,
    Ambient,
    Emissive,
    Height,
    Normals,
    Shininess,
    Opacity,
    Displacement,
    Lightmap,
    Reflection,
    BaseColor,
    NormalCamera,
    EmissionColor,
    Metalness,
    DiffuseRoughness,
    AmbientOcclusion,
    Sheen,
    Clearcoat,
    Transmission,
    MayaBase,
    MayaSpecular,
    MayaSpecularColor,
    MayaSpecularRoughness,
    Anisotropy,
    GltfMetallicRoughness,
};

enum class MaterialBlendFunction : Uint8 {
    Unknown = 0,
    Default,
    Additive,
};

enum class MaterialAlphaMode : Uint8 {
    Unknown = 0,
    Opaque,
    Mask,
    Blend,
};

enum class MaterialShadingModel : Uint8 {
    Unknown = 0,
    Flat,
    Gouraud,
    Phong,
    Blinn,
    Toon,
    OrenNayar,
    Minnaert,
    CookTorrance,
    Unlit,
    Fresnel,
    Pbr,
};

/// @brief YAML-authorable material data before texture paths resolve to runtime resources.
struct MaterialYamlRecord {
    String Name = {};

    hlslpp::float3 Ambient     = hlslpp::float3{0.0f, 0.0f, 0.0f};
    hlslpp::float3 Diffuse     = hlslpp::float3{0.62f, 0.28f, 0.10f};
    hlslpp::float3 Specular    = hlslpp::float3{0.0f, 0.0f, 0.0f};
    hlslpp::float3 Emissive    = hlslpp::float3{0.0f, 0.0f, 0.0f};
    hlslpp::float3 Transparent = hlslpp::float3{0.0f, 0.0f, 0.0f};
    hlslpp::float3 Reflective  = hlslpp::float3{0.0f, 0.0f, 0.0f};

    Float32 Opacity = 1.0f;
    Float32 BumpScaling = 1.0f;
    Float32 Shininess = 0.0f;
    Float32 ShininessStrength = 1.0f;
    Float32 RefractionIndex = 1.0f;
    Float32 Reflectivity = 0.0f;
    bool EnableWireframe = false;
    bool TwoSided = false;
    MaterialBlendFunction BlendFunction = MaterialBlendFunction::Default;
    MaterialAlphaMode AlphaMode = MaterialAlphaMode::Opaque;
    Float32 AlphaCutoff = 0.5f;
    MaterialShadingModel ShadingModel = MaterialShadingModel::Unknown;

    hlslpp::float4 BaseColorFactor = hlslpp::float4{1.0f, 1.0f, 1.0f, 1.0f};
    Float32        MetallicFactor = 0.0f;
    Float32        RoughnessFactor = 0.42f;
    hlslpp::float3 SheenColorFactor = hlslpp::float3{0.0f, 0.0f, 0.0f};
    Float32        SheenRoughnessFactor = 0.0f;
    Float32        ClearcoatFactor = 0.0f;
    Float32        ClearcoatRoughnessFactor = 0.0f;
    Float32        TransmissionFactor = 0.0f;
    Float32        VolumeThicknessFactor = 0.0f;
    Float32        VolumeAttenuationDistance = 0.0f;
    hlslpp::float3 VolumeAttenuationColor = hlslpp::float3{1.0f, 1.0f, 1.0f};

    String BaseColorTexture = {};
    String NormalTexture = {};
    String MetallicRoughnessTexture = {};
    String MetallicTexture = {};
    String RoughnessTexture = {};
    String OcclusionTexture = {};
    String EmissiveTexture = {};

    // Enum fields bridge to the YAML string vocabulary so generic meta assignment works.
    // Unknown strings keep the current value.
    auto SetBlendFunction(String Value) -> void {
        if (const auto Parsed = magic_enum::enum_cast<MaterialBlendFunction>(Value, magic_enum::case_insensitive)) {
            BlendFunction = *Parsed;
            return;
        }
        LogWarning("MaterialYamlRecord: unknown blend function '{}'; keeping current value", Value);
    }

    [[nodiscard]] auto GetBlendFunction() const -> String {
        return String(magic_enum::enum_name(BlendFunction));
    }

    auto SetAlphaMode(String Value) -> void {
        if (const auto Parsed = magic_enum::enum_cast<MaterialAlphaMode>(Value, magic_enum::case_insensitive)) {
            AlphaMode = *Parsed;
            return;
        }
        LogWarning("MaterialYamlRecord: unknown alpha mode '{}'; keeping current value", Value);
    }

    [[nodiscard]] auto GetAlphaMode() const -> String {
        return String(magic_enum::enum_name(AlphaMode));
    }

    auto SetShadingModel(String Value) -> void {
        if (const auto Parsed = magic_enum::enum_cast<MaterialShadingModel>(Value, magic_enum::case_insensitive)) {
            ShadingModel = *Parsed;
            return;
        }
        LogWarning("MaterialYamlRecord: unknown shading model '{}'; keeping current value", Value);
    }

    [[nodiscard]] auto GetShadingModel() const -> String {
        return String(magic_enum::enum_name(ShadingModel));
    }
};

/// @brief Runtime texture data cached by normalized asset path.
struct TextureData {
    Path                      AssetPath = {};
    RHIRef<RHISampledTexture> Texture   = nullptr;
};

using TextureDataHandle = entt::resource<TextureData>;

/// @brief Runtime representation of one material texture slot.
struct TextureRecord {
    TextureType       Type      = TextureType::Unknown;
    TextureDataHandle Texture   = {};
    TextureMapping    Mapping   = TextureMapping::UV;
    Uint32            UVIndex   = 0;
    Float32           Blend     = 1.0f;
    TextureOperation  Operation = TextureOperation::Multiply;
    TextureMapMode    MapModeU  = TextureMapMode::Wrap;
    TextureMapMode    MapModeV  = TextureMapMode::Wrap;
    TextureMapMode    MapModeW  = TextureMapMode::Wrap;
    Uint32            Flags     = 0;
};

/// @brief Material loader that created a runtime material record.
enum class MaterialSource {
    Unknown = 0,
    Yaml,
    Assimp,
};

/// @brief Renderer-neutral material record with source provenance.
struct MaterialRecord {
    MaterialSource Source      = MaterialSource::Unknown;
    Path           SourceAsset = {};
    String         Name        = {};

    // Common aiMaterial scalar and color properties.
    hlslpp::float3 Ambient     = hlslpp::float3{0.0f, 0.0f, 0.0f};
    hlslpp::float3 Diffuse     = hlslpp::float3{0.62f, 0.28f, 0.10f};
    hlslpp::float3 Specular    = hlslpp::float3{0.0f, 0.0f, 0.0f};
    hlslpp::float3 Emissive    = hlslpp::float3{0.0f, 0.0f, 0.0f};
    hlslpp::float3 Transparent = hlslpp::float3{0.0f, 0.0f, 0.0f};
    hlslpp::float3 Reflective  = hlslpp::float3{0.0f, 0.0f, 0.0f};

    Float32              Opacity           = 1.0f;
    Float32              BumpScaling       = 1.0f;
    Float32              Shininess         = 0.0f;
    Float32              ShininessStrength = 1.0f;
    Float32              RefractionIndex  = 1.0f;
    Float32              Reflectivity      = 0.0f;
    bool                 EnableWireframe  = false;
    bool                 TwoSided         = false;
    MaterialBlendFunction BlendFunction   = MaterialBlendFunction::Default;
    MaterialAlphaMode     AlphaMode       = MaterialAlphaMode::Opaque;
    Float32               AlphaCutoff     = 0.5f;
    MaterialShadingModel  ShadingModel    = MaterialShadingModel::Unknown;

    // PBR extensions commonly emitted by glTF and other modern importers.
    hlslpp::float4 BaseColorFactor            = hlslpp::float4{1.0f, 1.0f, 1.0f, 1.0f};
    Float32        MetallicFactor             = 0.0f;
    Float32        RoughnessFactor            = 0.42f;
    hlslpp::float3 SheenColorFactor           = hlslpp::float3{0.0f, 0.0f, 0.0f};
    Float32        SheenRoughnessFactor       = 0.0f;
    Float32        ClearcoatFactor            = 0.0f;
    Float32        ClearcoatRoughnessFactor   = 0.0f;
    Float32        TransmissionFactor         = 0.0f;
    Float32        VolumeThicknessFactor     = 0.0f;
    Float32        VolumeAttenuationDistance = 0.0f;
    hlslpp::float3 VolumeAttenuationColor    = hlslpp::float3{1.0f, 1.0f, 1.0f};

    // Imported texture slots with Material-owned semantic types.
    std::vector<TextureRecord> Textures = {};

    /// @brief GPU-side material ABI used by renderer transient buffers.
    struct alignas(16) GpuData {
        alignas(16) hlslpp::interop::float3 Ambient = hlslpp::interop::float3{
            hlslpp::float3{0.0f, 0.0f, 0.0f}};
        alignas(16) hlslpp::interop::float3 Diffuse = hlslpp::interop::float3{
            hlslpp::float3{0.62f, 0.28f, 0.10f}};
        alignas(16) hlslpp::interop::float3 Specular = hlslpp::interop::float3{
            hlslpp::float3{0.0f, 0.0f, 0.0f}};
        alignas(16) hlslpp::interop::float3 Emissive = hlslpp::interop::float3{
            hlslpp::float3{0.0f, 0.0f, 0.0f}};
        alignas(16) hlslpp::interop::float3 Transparent = hlslpp::interop::float3{
            hlslpp::float3{0.0f, 0.0f, 0.0f}};
        alignas(16) hlslpp::interop::float3 Reflective = hlslpp::interop::float3{
            hlslpp::float3{0.0f, 0.0f, 0.0f}};

        Float32 Opacity           = 1.0f;
        Float32 BumpScaling       = 1.0f;
        Float32 Shininess         = 0.0f;
        Float32 ShininessStrength = 1.0f;
        Float32 RefractionIndex   = 1.0f;
        Float32 Reflectivity      = 0.0f;
        Uint32  EnableWireframe   = 0;
        Uint32  TwoSided          = 0;
        Uint32  BlendFunction     = 0;
        Uint32  ShadingModel      = 0;
        Uint32  AlphaMode         = 0;
        Float32 AlphaCutoff       = 0.5f;

        alignas(16) hlslpp::interop::float4 BaseColorFactor = hlslpp::interop::float4{
            hlslpp::float4{1.0f, 1.0f, 1.0f, 1.0f}};
        Float32        MetallicFactor             = 0.0f;
        Float32        RoughnessFactor            = 0.42f;
        alignas(16) hlslpp::interop::float3 SheenColorFactor = hlslpp::interop::float3{
            hlslpp::float3{0.0f, 0.0f, 0.0f}};
        Float32 SheenRoughnessFactor       = 0.0f;
        Float32 ClearcoatFactor            = 0.0f;
        Float32 ClearcoatRoughnessFactor   = 0.0f;
        Float32 TransmissionFactor         = 0.0f;
        Float32 VolumeThicknessFactor     = 0.0f;
        Float32 VolumeAttenuationDistance = 0.0f;
        alignas(16) hlslpp::interop::float3 VolumeAttenuationColor = hlslpp::interop::float3{
            hlslpp::float3{1.0f, 1.0f, 1.0f}};

        // Texture handles use {descriptor slot, sampler slot}; {0, 0} is the
        // null handle, and RHIRefArray reserves descriptor slot 0. std430
        // aligns uint2 to 8 bytes, so every handle must be 8-aligned to match
        // the Slang-side MaterialRecord layout.
        alignas(8) std::array<Uint32, 2> BaseColorTexture         = {};
        alignas(8) std::array<Uint32, 2> NormalTexture            = {};
        alignas(8) std::array<Uint32, 2> MetallicRoughnessTexture = {};
        alignas(8) std::array<Uint32, 2> MetallicTexture          = {};
        alignas(8) std::array<Uint32, 2> RoughnessTexture         = {};
        alignas(8) std::array<Uint32, 2> OcclusionTexture         = {};
        alignas(8) std::array<Uint32, 2> EmissiveTexture          = {};
    };

    [[nodiscard]] auto BuildGpuData(const RHIRefArray<RHISampledTexture>& TextureArray) const
        -> GpuData {
        const auto FirstSlot = [this](TextureType Type) -> TextureDataHandle {
            const auto Slot = std::ranges::find_if(Textures, [Type](const TextureRecord& Record) -> bool {
                return Record.Type == Type;
            });
            return Slot == Textures.end() ? TextureDataHandle{} : Slot->Texture;
        };
        const auto ResolveTexture = [&TextureArray](const TextureDataHandle& Texture) -> std::array<Uint32, 2> {
            if (!Texture || !Texture->Texture)
                return {};
            const auto Index = TextureArray.FindIndex(Texture->Texture);
            return Index ? std::array<Uint32, 2>{*Index, 0} : std::array<Uint32, 2>{};
        };

        GpuData Result{
            .Ambient                    = hlslpp::interop::float3{Ambient},
            .Diffuse                    = hlslpp::interop::float3{Diffuse},
            .Specular                   = hlslpp::interop::float3{Specular},
            .Emissive                   = hlslpp::interop::float3{Emissive},
            .Transparent                = hlslpp::interop::float3{Transparent},
            .Reflective                 = hlslpp::interop::float3{Reflective},
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
            const auto Metallic  = FirstSlot(TextureType::Metalness);
            const auto Roughness = FirstSlot(TextureType::DiffuseRoughness);
            const bool SharedMetallicRoughness =
                Metallic && Roughness && Metallic->Texture == Roughness->Texture;
            if (SharedMetallicRoughness) {
                Result.MetallicRoughnessTexture = ResolveTexture(Metallic);
            } else {
                Result.MetallicTexture = ResolveTexture(Metallic);
                Result.RoughnessTexture = ResolveTexture(Roughness);
            }
        }
        Result.OcclusionTexture = ResolveTexture(FirstSlot(TextureType::AmbientOcclusion));
        Result.EmissiveTexture   = ResolveTexture(EmissiveSlot);
        return Result;
    }
};

static_assert(sizeof(MaterialRecord::GpuData) == 304);
static_assert(offsetof(MaterialRecord::GpuData, Ambient) == 0);
static_assert(offsetof(MaterialRecord::GpuData, Diffuse) == 16);
static_assert(offsetof(MaterialRecord::GpuData, Specular) == 32);
static_assert(offsetof(MaterialRecord::GpuData, Emissive) == 48);
static_assert(offsetof(MaterialRecord::GpuData, Opacity) == 92);
static_assert(offsetof(MaterialRecord::GpuData, AlphaMode) == 132);
static_assert(offsetof(MaterialRecord::GpuData, AlphaCutoff) == 136);
static_assert(offsetof(MaterialRecord::GpuData, BaseColorFactor) == 144);
static_assert(offsetof(MaterialRecord::GpuData, SheenColorFactor) == 176);
static_assert(offsetof(MaterialRecord::GpuData, VolumeAttenuationColor) == 224);
// Handle offsets mirror the Slang MaterialRecord std430 layout (uint2 aligns
// to 8, so the first handle starts at 240, not 236).
static_assert(offsetof(MaterialRecord::GpuData, BaseColorTexture) == 240);
static_assert(offsetof(MaterialRecord::GpuData, NormalTexture) == 248);
static_assert(offsetof(MaterialRecord::GpuData, MetallicRoughnessTexture) == 256);
static_assert(offsetof(MaterialRecord::GpuData, MetallicTexture) == 264);
static_assert(offsetof(MaterialRecord::GpuData, RoughnessTexture) == 272);
static_assert(offsetof(MaterialRecord::GpuData, OcclusionTexture) == 280);
static_assert(offsetof(MaterialRecord::GpuData, EmissiveTexture) == 288);

} // namespace SoulEngine

// NOTE: entt meta registration for MaterialYamlRecord must live in this TU,
// the partition that defines the type. Clang 22.1.3 and 23.1.1 ICE
// (SEGV 0xC0000005 while mangling entt::meta_factory<T>::data) when the
// registered type is imported from another partition; keeping the
// registration beside the type avoids that path entirely. The split across
// these helpers additionally keeps clang 22.1.x debug builds (-g) out of a
// debug-info-generation ICE that a single 37-field registration function hits.
namespace SoulEngine {
namespace {

auto RegisterCoreScalarYamlMeta(entt::meta_factory<MaterialYamlRecord>& Factory) -> void {
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
}

auto RegisterBlendAndShadingYamlMeta(entt::meta_factory<MaterialYamlRecord>& Factory) -> void {
    Factory.data<&MaterialYamlRecord::SetBlendFunction, &MaterialYamlRecord::GetBlendFunction>("blend_function");
    Factory.data<&MaterialYamlRecord::SetShadingModel, &MaterialYamlRecord::GetShadingModel>("shading_model");
    Factory.data<&MaterialYamlRecord::SetAlphaMode, &MaterialYamlRecord::GetAlphaMode>("alpha_mode");
    Factory.data<&MaterialYamlRecord::AlphaCutoff>("alpha_cutoff");
}

auto RegisterPbrYamlMeta(entt::meta_factory<MaterialYamlRecord>& Factory) -> void {
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
}

auto RegisterTextureYamlMeta(entt::meta_factory<MaterialYamlRecord>& Factory) -> void {
    Factory.data<&MaterialYamlRecord::BaseColorTexture>("base_color_texture");
    Factory.data<&MaterialYamlRecord::NormalTexture>("normal_texture");
    Factory.data<&MaterialYamlRecord::MetallicRoughnessTexture>("metallic_roughness_texture");
    Factory.data<&MaterialYamlRecord::MetallicTexture>("metallic_texture");
    Factory.data<&MaterialYamlRecord::RoughnessTexture>("roughness_texture");
    Factory.data<&MaterialYamlRecord::OcclusionTexture>("occlusion_texture");
    Factory.data<&MaterialYamlRecord::EmissiveTexture>("emissive_texture");
}

auto RegisterMaterialYamlMeta() -> void {
    entt::meta_factory<MaterialYamlRecord> Factory = entt::meta_factory<MaterialYamlRecord>{}.type("material");
    RegisterCoreScalarYamlMeta(Factory);
    RegisterBlendAndShadingYamlMeta(Factory);
    RegisterPbrYamlMeta(Factory);
    RegisterTextureYamlMeta(Factory);
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
