module;

#include <entt/entt.hpp>
#include <hlsl++.h>

export module Scene:Mesh;

export import Core;
import Material;

export namespace SoulEngine {

/// @brief Scene-authored mesh asset reference.
///
/// Paths are relative to the current application Assets directory. Renderer-specific
/// mesh resources, uploads, and draw representations are
/// owned by each renderer rather than this component.
struct MeshComponent {
    String Asset    = {};
    /// Scene-local PBR material instance ID. Empty uses the built-in material defaults.
    String Material = {};
};

/// @brief Immutable CPU render record for one mesh asset instance.
///
/// It intentionally carries only renderer-neutral asset identity and instance
/// state. Each renderer resolves it to its own GPU representation.
struct RenderableInstance {
    String                       MeshAsset      = {};
    /// Scene-local material instance ID. Empty identifies the shared built-in material.
    String                       MaterialId     = {};
    PbrMetallicRoughnessMaterial Material       = {};
    hlslpp::float4x4             WorldTransform = hlslpp::float4x4::identity();
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
