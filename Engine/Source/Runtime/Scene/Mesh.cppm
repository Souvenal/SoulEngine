module;

#include <entt/entt.hpp>
#include <hlsl++.h>

export module Scene:Mesh;

export import Core;

export namespace SoulEngine {

/// @brief Scene-authored mesh asset reference.
///
/// Paths are relative to the current application Assets directory. Renderer-specific
/// mesh resources, uploads, and draw representations are
/// owned by each renderer rather than this component.
struct MeshComponent {
    String Asset    = {};
    /// Scene-local PBR material instance ID (registered in MaterialManager on scene load).
    /// Empty falls back to the mesh-imported material instance, then the built-in default.
    String Material = {};
};

/// @brief Immutable scene snapshot information for one mesh entity.
///
/// Each renderer resolves this renderer-neutral data into its own draw representation.
struct MeshInfo {
    Uint32           EntityId       = 0;
    String           MeshAsset      = {};
    /// Scene-local material instance ID. Empty falls back to the mesh-imported
    /// material instance, then the built-in default.
    String           MaterialId     = {};
    hlslpp::float4x4 WorldTransform = hlslpp::float4x4::identity();
};

} // namespace SoulEngine

namespace SoulEngine {

namespace {

struct MeshComponentMetaRegistration {
    MeshComponentMetaRegistration() {
        entt::meta_factory<MeshComponent>{}
            .type("mesh")
            .data<&MeshComponent::Asset>("asset")
            .data<&MeshComponent::Material>("material");
    }
};

MeshComponentMetaRegistration g_MeshComponentMetaRegistration = {};

} // namespace

} // namespace SoulEngine
