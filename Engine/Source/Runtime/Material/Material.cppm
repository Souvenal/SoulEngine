module;

#include <cstddef>
#include <hlsl++.h>

export module Material;

import std;

export import Core;

export namespace SoulEngine {

/// @brief Renderer-neutral parameters for the built-in metallic-roughness PBR material.
struct PbrMaterial {
    hlslpp::float3 BaseColor                = hlslpp::float3(0.62f, 0.28f, 0.10f);
    float          Metallic                 = 0.0f;
    float          Roughness                = 0.42f;
    hlslpp::float3 Emissive                 = hlslpp::float3(0.0f, 0.0f, 0.0f);
    /// Optional base-color texture path. Scene resolves it to an asset identity in snapshots.
    String         BaseColorTexture         = {};
    /// Optional texture asset paths. Scene resolves scene-authored paths in snapshots.
    String         NormalTexture            = {};
    String         MetallicRoughnessTexture = {};
    String         MetallicTexture          = {};
    String         RoughnessTexture         = {};
    String         OcclusionTexture         = {};
    String         EmissiveTexture          = {};
};

/// @brief GPU-side material record for StructuredBuffer.
/// Contains PBR factors and bindless texture indices.
struct alignas(16) MaterialRecord {
    alignas(16) hlslpp::interop::float4 BaseColorFactor = hlslpp::interop::float4{
        hlslpp::float4{1.0f, 1.0f, 1.0f, 1.0f}};
    alignas(16) hlslpp::interop::float3 EmissiveFactor = hlslpp::interop::float3{hlslpp::float3{0.0f, 0.0f, 0.0f}};
    Float32 MetallicFactor                             = 0.0f;
    Float32 RoughnessFactor                            = 0.42f;
    Float32 OcclusionFactor                            = 1.0f;
    Float32 Padding                                    = 0.0f;

    Int32 BaseColorTexture         = -1;
    Int32 NormalTexture            = -1;
    Int32 MetallicRoughnessTexture = -1;
    Int32 MetallicTexture          = -1;
    Int32 RoughnessTexture         = -1;
    Int32 OcclusionTexture         = -1;
    Int32 EmissiveTexture          = -1;
    Int32 Padding2                 = -1;
};
static_assert(sizeof(MaterialRecord) == 80);
static_assert(offsetof(MaterialRecord, BaseColorFactor) == 0);
static_assert(offsetof(MaterialRecord, EmissiveFactor) == 16);
static_assert(offsetof(MaterialRecord, MetallicFactor) == 28);
static_assert(offsetof(MaterialRecord, BaseColorTexture) == 44);
static_assert(offsetof(MaterialRecord, EmissiveTexture) == 68);

} // namespace SoulEngine
