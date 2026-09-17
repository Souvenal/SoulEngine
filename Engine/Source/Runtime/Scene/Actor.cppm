/// @file   Actor.cppm
/// @brief  Actor mechanism: the preset-actor base class.
module;

#include <entt/entt.hpp>

export module Scene:Actor;

import std;
export import Core;

export namespace SoulEngine {

/// @brief Base class for preset actors — entities created from code presets
///        rather than the Scene Document's entity list.
///
/// An Actor wraps one registry entity. Unlike Scene Entities (see CONTEXT.md),
/// Actors are not necessarily spatial: the scene-settings actor carries no
/// Transform, ParentComponent, or ChildrenComponent. Creation goes through
/// `registry.create()` directly, bypassing `Scene::CreateEntity`.
class AActor {
  public:
    virtual ~AActor() = default;
    AActor(const AActor&)            = delete;
    auto operator=(const AActor&) -> AActor& = delete;
    AActor(AActor&&)                 = delete;
    auto operator=(AActor&&) -> AActor& = delete;

    /// @brief The wrapped registry entity.
    [[nodiscard]] auto GetEntity() const -> entt::entity { return m_Entity; }

  protected:
    /// @brief Create the backing entity directly in the registry — no
    /// Transform, no hierarchy attachment.
    explicit AActor(entt::registry& Registry) : m_Registry(Registry), m_Entity(Registry.create()) {}

    [[nodiscard]] auto GetRegistry() const -> entt::registry& { return m_Registry; }

    entt::registry& m_Registry;
    entt::entity    m_Entity = entt::null;
};

} // namespace SoulEngine
