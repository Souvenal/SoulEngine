/// @file   EnvironmentMap.cppm
/// @brief  Scene-level environment lighting component and its meta registration.
module;

#include <entt/entt.hpp>

export module Scene:EnvironmentMap;

import std;
export import Core;

export namespace SoulEngine {

/// @brief Scene-level environment lighting declaration.
///
/// Authoring state mirrored from the Scene Document's optional root
/// `env_map` field. The asset handle is registered through the AssetManager
/// when the document declares it; an invalid handle (document said nothing,
/// or registration failed) means the scene runs without environment lighting.
/// GPU resources stay renderer-owned.
struct EnvironmentMapComponent {
    AssetHandle Asset     = {};
    Float32     Intensity = 1.0f;

    /// @brief Register the declared asset path and store its handle.
    ///
    /// YAML bridge for the meta field "asset": the document carries a path
    /// relative to the application's Assets directory — the same scene-asset
    /// convention mesh assets use — while the AssetManager resolves against
    /// the application root, so the Assets level is joined here. Takes
    /// `const String&` so entt meta assigns the decoded YAML string without
    /// a view conversion.
    auto SetAssetPath(const String& AssetPath) -> void {
        Asset = AssetManager::Get().Load(Path{"Assets"} / Path{AssetPath});
    }

    /// @brief Meta getter paired with SetAssetPath. entt meta derives a
    /// settable field's type from the getter's return type, so a setter-only
    /// binding would type the field as void and fail YAML dispatch. Returns
    /// the registered full path (empty until set).
    [[nodiscard]] auto GetAssetPath() const -> String {
        return AssetManager::Get().GetPath(Asset).string();
    }
};

} // namespace SoulEngine

namespace SoulEngine {

// Note: this namespace must stay named. In a named module, clang 23 silently
// drops the dynamic initializer of an unreferenced anonymous-namespace variable,
// which would skip this entt meta registration at program startup.
namespace MetaRegistration {

using namespace entt::literals;

struct EnvironmentMapComponentMetaRegistration {
    EnvironmentMapComponentMetaRegistration() {
        entt::meta_factory<EnvironmentMapComponent>{"env_map"_hs}
            .data<&EnvironmentMapComponent::SetAssetPath, &EnvironmentMapComponent::GetAssetPath>("asset"_hs)
            .data<&EnvironmentMapComponent::Intensity>("intensity"_hs)
            .func<&EmplaceComponent<EnvironmentMapComponent>>("emplace"_hs);
    }
};

EnvironmentMapComponentMetaRegistration g_EnvironmentMapComponentMetaRegistration = {};

} // namespace MetaRegistration

} // namespace SoulEngine
