module;

#include <entt/entt.hpp>
#include <hlsl++.h>

module Core.ECS.Transform;

import std;

namespace SoulEngine {

[[nodiscard]] auto TransformComponentFields() -> FieldTable {
    static constexpr FieldDescriptor Table[] = {
        Field<&TransformComponent::Translation>("translation"),
        Field<&TransformComponent::Rotation>("rotation"),
        Field<&TransformComponent::Scale>("scale"),
    };
    return Table;
}

[[nodiscard]] auto TransformComponent::GetLocalMatrix() const -> hlslpp::float4x4 {
    const auto TranslationMatrix = hlslpp::float4x4::translation(Translation);
    const auto RotationX         = hlslpp::float4x4::rotation_x(Rotation.x * std::numbers::pi_v<float> / 180.0f);
    const auto RotationY         = hlslpp::float4x4::rotation_y(Rotation.y * std::numbers::pi_v<float> / 180.0f);
    const auto RotationZ         = hlslpp::float4x4::rotation_z(Rotation.z * std::numbers::pi_v<float> / 180.0f);
    const auto RotationMatrix    = hlslpp::mul(hlslpp::mul(RotationX, RotationY), RotationZ);
    const auto ScaleMatrix       = hlslpp::float4x4::scale(Scale);
    return hlslpp::mul(hlslpp::mul(ScaleMatrix, RotationMatrix), TranslationMatrix);
}

auto TransformSystem::OnUpdate(Float32 DeltaTime) -> void {
    auto& DirtyTransforms = GetDirtyTransforms();
    using QueueEntry      = std::pair<Uint32, entt::entity>;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<>> Queue;
    std::unordered_set<entt::entity>                                         Scheduled;

    const auto Schedule = [&](const entt::entity Entity) -> void {
        // Return value of emplace is `std::pair<iterator, bool>`, where the bool indicates whether the
        // insertion took place.
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

auto TransformSystem::SetupObservers() -> void {
    auto& DirtyTransforms = GetDirtyTransforms();
    DirtyTransforms.reset();
    DirtyTransforms.clear();
    DirtyTransforms.on_construct<TransformComponent>().on_update<TransformComponent>();
}

auto TransformSystem::TeardownObservers() -> void {
    auto& DirtyTransforms = GetDirtyTransforms();
    DirtyTransforms.reset();
    DirtyTransforms.clear();
}

auto TransformSystem::ClearObservers() -> void {
    GetDirtyTransforms().clear();
}

[[nodiscard]] auto TransformSystem::GetDirtyTransforms() const -> entt::storage_type_t<entt::reactive>& {
    return m_Registry.storage<entt::reactive>(DirtyTransformStorageId);
}

} // namespace SoulEngine
