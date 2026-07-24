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
};

} // namespace SoulEngine
