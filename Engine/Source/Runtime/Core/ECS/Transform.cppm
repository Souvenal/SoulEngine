module;

#include <entt/entt.hpp>
#include <hlsl++.h>

export module Core.ECS.Transform;

export import Core.ECS;
export import Core.Util;

import std;

namespace SoulEngine {

/// @brief Transform component for spatial entities.
///
/// Contains local translation, rotation, and scale. World transform is derived through the entity hierarchy.
export struct TransformComponent {
    hlslpp::float3   Translation    = hlslpp::float3(0.0f, 0.0f, 0.0f);
    hlslpp::float3   Rotation       = hlslpp::float3(0.0f, 0.0f, 0.0f); ///< Euler angles in degrees
    hlslpp::float3   Scale          = hlslpp::float3(1.0f, 1.0f, 1.0f);
    hlslpp::float4x4 WorldTransform = hlslpp::float4x4::identity();

    /// @brief Get the local transform matrix from translation, rotation, and scale.
    ///
    /// Rotation composes as Rx * Ry * Rz (row-vector convention): the X, Y, then Z rotations are applied
    /// about the fixed parent-space axes.
    [[nodiscard]] auto GetLocalMatrix() const -> hlslpp::float4x4;
};

/// @brief System for updating transform hierarchies.
///
/// Updates world transforms based on parent-child relationships. Uses registry-owned reactive storage to
/// track dirty transforms.
export class TransformSystem : public ISystem {
  public:
    explicit TransformSystem(entt::registry& Registry) : ISystem(Registry) {}
    ~TransformSystem() override = default;

    /// @brief Update dirty transform hierarchies.
    /// @param DeltaTime Time elapsed since last frame in seconds.
    auto OnUpdate(Float32 DeltaTime) -> void override;

    /// @brief Set up dirty-transform tracking for TransformComponent changes.
    auto SetupObservers() -> void override;

    /// @brief Tear down dirty-transform tracking.
    auto TeardownObservers() -> void override;

    /// @brief Clear observer state without disconnecting.
    auto ClearObservers() -> void override;

  private:
    static constexpr entt::id_type DirtyTransformStorageId = entt::hashed_string{"SoulEngine.TransformDirty"}.value();

    /// @brief Return the registry-owned dirty transform storage.
    [[nodiscard]] auto GetDirtyTransforms() const -> entt::storage_type_t<entt::reactive>&;
};

} // namespace SoulEngine

namespace SoulEngine {

// Note: this namespace must stay named. In a named module, clang 23 silently drops the dynamic initializer of
// an unreferenced anonymous-namespace variable, which would skip this entt meta registration at program
// startup.
namespace MetaRegistration {

using namespace entt::literals;

struct TransformComponentMetaRegistration {
    TransformComponentMetaRegistration() {
        entt::meta_factory<TransformComponent>{"transform"_hs}
            .data<&TransformComponent::Translation>("translation"_hs)
            .data<&TransformComponent::Rotation>("rotation"_hs)
            .data<&TransformComponent::Scale>("scale"_hs)
            .func<&EmplaceComponent<TransformComponent>>("emplace"_hs);
    }
};

TransformComponentMetaRegistration g_TransformComponentMetaRegistration = {};

} // namespace MetaRegistration

} // namespace SoulEngine
