module;

#include <entt/entt.hpp>
#include <hlsl++.h>

export module Core:ECS.Transform;

export import :ECS;
export import :Util;

import std;

export namespace SoulEngine {

/// @brief Transform component for spatial entities.
///
/// Contains local translation, rotation, and scale.
/// World transform is derived through the entity hierarchy.
struct TransformComponent {
    hlslpp::float3   Translation    = hlslpp::float3(0.0f, 0.0f, 0.0f);
    hlslpp::float3   Rotation       = hlslpp::float3(0.0f, 0.0f, 0.0f); ///< Euler angles in degrees
    hlslpp::float3   Scale          = hlslpp::float3(1.0f, 1.0f, 1.0f);
    hlslpp::float4x4 WorldTransform = hlslpp::float4x4::identity();

    /// @brief Get the local transform matrix from translation, rotation, and scale.
    ///
    /// Rotation composes as Rx * Ry * Rz (row-vector convention): the X, Y,
    /// then Z rotations are applied about the fixed parent-space axes.
    [[nodiscard]] auto GetLocalMatrix() const -> hlslpp::float4x4 {
        const auto TranslationMatrix = hlslpp::float4x4::translation(Translation);

        const auto RotationX      = hlslpp::float4x4::rotation_x(Rotation.x * std::numbers::pi_v<float> / 180.0f);
        const auto RotationY      = hlslpp::float4x4::rotation_y(Rotation.y * std::numbers::pi_v<float> / 180.0f);
        const auto RotationZ      = hlslpp::float4x4::rotation_z(Rotation.z * std::numbers::pi_v<float> / 180.0f);
        const auto RotationMatrix = hlslpp::mul(hlslpp::mul(RotationX, RotationY), RotationZ);

        const auto ScaleMatrix = hlslpp::float4x4::scale(Scale);

        return hlslpp::mul(hlslpp::mul(ScaleMatrix, RotationMatrix), TranslationMatrix);
    }
};

/// @brief System for updating transform hierarchies.
///
/// Updates world transforms based on parent-child relationships.
/// Uses registry-owned reactive storage to track dirty transforms.
class TransformSystem : public ISystem {
  public:
    explicit TransformSystem(entt::registry& Registry) : ISystem(Registry) {}
    ~TransformSystem() override = default;

    /// @brief Update dirty transform hierarchies.
    /// @param DeltaTime Time elapsed since last frame in seconds.
    auto OnUpdate(Float32 DeltaTime) -> void override {
        auto& DirtyTransforms = GetDirtyTransforms();
        using QueueEntry      = std::pair<Uint32, entt::entity>;
        std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<>> Queue;
        std::unordered_set<entt::entity>                                         Scheduled;

        const auto Schedule = [&](const entt::entity Entity) -> void {
            // Return value of emplace is `std::pair<iterator, bool>`,
            // where the bool indicates whether the insertion took place.
            //
            // I didn't know that before.
            if (!Scheduled.emplace(Entity).second)
                return;

            const auto Depth = m_Registry.get<ParentComponent>(Entity).Depth;
            Queue.emplace(Depth, Entity);
        };

        for (const auto Entity : DirtyTransforms)
            Schedule(Entity);

        while (!Queue.empty()) {
            const auto Entity = Queue.top().second;
            Queue.pop();

            // The hidden root has no ParentComponent but does have TransformComponent, so every parent is
            // transformable.
            const auto Parent    = m_Registry.get<ParentComponent>(Entity).Parent;
            auto&      Transform = m_Registry.get<TransformComponent>(Entity);
            Transform.WorldTransform =
                hlslpp::mul(Transform.GetLocalMatrix(), m_Registry.get<TransformComponent>(Parent).WorldTransform);

            if (!m_Registry.all_of<ChildrenComponent>(Entity))
                continue;

            for (const auto Child : m_Registry.get<ChildrenComponent>(Entity).Children)
                Schedule(Child);
        }
    }

    /// @brief Set up dirty-transform tracking for TransformComponent changes.
    auto SetupObservers() -> void override {
        auto& DirtyTransforms = GetDirtyTransforms();
        DirtyTransforms.reset();
        DirtyTransforms.clear();
        DirtyTransforms.on_construct<TransformComponent>().on_update<TransformComponent>();
    }

    /// @brief Tear down dirty-transform tracking.
    auto TeardownObservers() -> void override {
        auto& DirtyTransforms = GetDirtyTransforms();
        DirtyTransforms.reset();
        DirtyTransforms.clear();
    }

    /// @brief Clear observer state without disconnecting.
    auto ClearObservers() -> void override {
        GetDirtyTransforms().clear();
    }

  private:
    static constexpr entt::id_type DirtyTransformStorageId = entt::hashed_string{"SoulEngine.TransformDirty"}.value();

    /// @brief Return the registry-owned dirty transform storage.
    [[nodiscard]] auto GetDirtyTransforms() const -> entt::storage_type_t<entt::reactive>& {
        return m_Registry.storage<entt::reactive>(DirtyTransformStorageId);
    }
};

} // namespace SoulEngine

namespace SoulEngine {

// Note: this namespace must stay named. In a named module, clang 23 silently
// drops the dynamic initializer of an unreferenced anonymous-namespace variable,
// which would skip this entt meta registration at program startup.
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
