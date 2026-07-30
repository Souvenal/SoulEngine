module;

#include <cstddef>
#include <hlsl++.h>

export module Material;

import std;

export import Core;

export namespace SoulEngine {

/// @brief Renderer-neutral parameters for the built-in metallic-roughness PBR material.
struct PbrMetallicRoughnessMaterial {
    hlslpp::float3 BaseColor = hlslpp::float3(0.62f, 0.28f, 0.10f);
    float          Metallic  = 0.0f;
    float          Roughness = 0.42f;
    /// Optional base-color texture path. Scene resolves it to an asset identity in snapshots.
    String         BaseColorTexture          = {};
    /// Optional texture asset paths. Scene resolves scene-authored paths in snapshots.
    String         NormalTexture             = {};
    String         MetallicRoughnessTexture  = {};
    String         MetallicTexture           = {};
    String         RoughnessTexture          = {};
    String         OcclusionTexture          = {};
    String         EmissiveTexture           = {};
    hlslpp::float3 Emissive                  = hlslpp::float3(0.0f, 0.0f, 0.0f);
};

/// @brief Renderer-neutral texture table indices for the built-in PBR material.
struct PbrMaterialTextureIndices {
    Int32 BaseColor          = -1;
    Int32 Normal             = -1;
    Int32 MetallicRoughness  = -1;
    Int32 Metallic           = -1;
    Int32 Roughness          = -1;
    Int32 Occlusion          = -1;
    Int32 Emissive           = -1;
};

/// @brief Shader storage-buffer ABI for a resolved metallic-roughness material.
struct alignas(16) PbrMaterialGpuData {
    alignas(16) hlslpp::interop::float4 BaseColorFactor =
        hlslpp::interop::float4{hlslpp::float4{0.62f, 0.28f, 0.10f, 1.0f}};
    alignas(16) hlslpp::interop::float4 EmissiveFactor =
        hlslpp::interop::float4{hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f}};
    Float32 MetallicFactor = 0.0f;
    Float32 RoughnessFactor = 0.42f;
    Int32 BaseColorTextureIndex = -1;
    Int32 NormalTextureIndex = -1;
    Int32 MetallicRoughnessTextureIndex = -1;
    Int32 MetallicTextureIndex = -1;
    Int32 RoughnessTextureIndex = -1;
    Int32 OcclusionTextureIndex = -1;
    Int32 EmissiveTextureIndex = -1;
};
static_assert(sizeof(PbrMaterialGpuData) == 80);
static_assert(offsetof(PbrMaterialGpuData, BaseColorFactor) == 0);
static_assert(offsetof(PbrMaterialGpuData, EmissiveFactor) == 16);
static_assert(offsetof(PbrMaterialGpuData, MetallicFactor) == 32);
static_assert(offsetof(PbrMaterialGpuData, BaseColorTextureIndex) == 40);
static_assert(offsetof(PbrMaterialGpuData, EmissiveTextureIndex) == 64);

[[nodiscard]] inline auto BuildPbrMaterialGpuData(const PbrMetallicRoughnessMaterial& Material,
                                                   PbrMaterialTextureIndices Indices) -> PbrMaterialGpuData {
    return PbrMaterialGpuData{
        .BaseColorFactor = hlslpp::interop::float4{
            hlslpp::float4{Material.BaseColor.x, Material.BaseColor.y, Material.BaseColor.z, 1.0f}},
        .EmissiveFactor = hlslpp::interop::float4{
            hlslpp::float4{Material.Emissive.x, Material.Emissive.y, Material.Emissive.z, 0.0f}},
        .MetallicFactor = Material.Metallic,
        .RoughnessFactor = Material.Roughness,
        .BaseColorTextureIndex = Indices.BaseColor,
        .NormalTextureIndex = Indices.Normal,
        .MetallicRoughnessTextureIndex = Indices.MetallicRoughness,
        .MetallicTextureIndex = Indices.Metallic,
        .RoughnessTextureIndex = Indices.Roughness,
        .OcclusionTextureIndex = Indices.Occlusion,
        .EmissiveTextureIndex = Indices.Emissive,
    };
}

} // namespace SoulEngine
