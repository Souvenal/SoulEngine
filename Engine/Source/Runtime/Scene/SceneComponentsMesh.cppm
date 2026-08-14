module;

#include <entt/entt.hpp>
#include <hlsl++.h>

export module Scene:Components.Mesh;

import Material;
import :Components.Core;

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

[[nodiscard]] auto ValidateMeshComponent(const SceneComponentValidationContext& Context,
                                         entt::registry&                        Registry,
                                         SceneEntity                            Entity,
                                         String&                                Error) -> bool {
    const auto* Mesh = Registry.try_get<MeshComponent>(Entity);
    if (!Mesh) {
        Error = "Mesh component metadata does not contain MeshComponent";
        return false;
    }
    if (Mesh->Asset.empty()) {
        Error = "asset must not be empty";
        return false;
    }
    if (Path(Mesh->Asset).is_absolute()) {
        Error = "asset must be relative to the current application Assets directory";
        return false;
    }
    if (!Mesh->Material.empty() &&
        (!Context.HasMaterialInstance || !Context.HasMaterialInstance(Context.UserData, Mesh->Material))) {
        Error = Format("material instance '{}' does not exist", Mesh->Material);
        return false;
    }
    return true;
}

struct MeshComponentMetaRegistration {
    MeshComponentMetaRegistration() {
        entt::meta_factory<MeshComponent>{}
            .type("mesh")
            .custom<SceneComponentSchema>(SceneComponentSchema{
                .Create   = &CreateSceneComponent<MeshComponent>,
                .Remove   = &RemoveSceneComponent<MeshComponent>,
                .Has      = &HasSceneComponent<MeshComponent>,
                .Validate = &ValidateMeshComponent,
            })
            .data<&MeshComponent::Asset>("asset")
            .data<&MeshComponent::Material>("material");
    }
};

MeshComponentMetaRegistration g_MeshComponentMetaRegistration = {};

} // namespace

} // namespace SoulEngine
