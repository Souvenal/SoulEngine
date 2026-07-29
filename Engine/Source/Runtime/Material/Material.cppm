module;

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

} // namespace SoulEngine
