/// @file   SceneSetting.cppm
/// @brief  The implicit scene-settings preset actor.
module;

#include <entt/entt.hpp>

export module Scene:SceneSetting;

import std;
export import Core;
import :Actor;
import :EnvironmentMap;

export namespace SoulEngine {

/// @brief Implicit scene-settings actor. Created automatically with every
/// Scene — never authored in the Scene Document's entity list — and hosts
/// scene-level settings components such as EnvironmentMapComponent.
class ASceneSetting final : public AActor {
  public:
    explicit ASceneSetting(entt::registry& Registry)
        : AActor(Registry) {
        m_Registry.emplace<EnvironmentMapComponent>(m_Entity);
    }

    /// @brief The settings component on this actor.
    [[nodiscard]] auto GetEnvironmentMap() -> EnvironmentMapComponent& {
        return m_Registry.get<EnvironmentMapComponent>(m_Entity);
    }
};

} // namespace SoulEngine
