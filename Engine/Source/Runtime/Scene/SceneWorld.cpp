module;

#include <entt/entt.hpp>

module Scene:World.impl;

import :World;

namespace SoulEngine {

auto Scene::ResolveAssetPath(StringView AssetPath) const -> String {
    const Path Asset{String(AssetPath)};
    if (Asset.is_absolute() || m_AssetRoot.empty())
        return Asset.lexically_normal().string();
    return (m_AssetRoot / Asset).lexically_normal().string();
}

auto Scene::CollectSnapshotInstances() const -> std::vector<InstanceRecord> {
    const auto MeshSys = m_SystemScheduler.Get<MeshSystem>();
    if (!MeshSys)
        return {};

    return MeshSys->get().CollectInstances();
}

auto Scene::CreateEntityInternal(String Name, entt::entity Parent) -> entt::entity {
    const auto Entity         = m_Registry.create();
    const auto ResolvedParent = Parent != entt::null && m_Registry.valid(Parent) ? Parent : m_RootEntity;

    m_Registry.emplace<ParentComponent>(Entity, ResolvedParent);
    if (ResolvedParent != m_RootEntity) {
        if (m_Registry.all_of<ChildrenComponent>(ResolvedParent)) {
            m_Registry.get<ChildrenComponent>(ResolvedParent).Children.emplace_back(Entity);
        } else {
            m_Registry.emplace<ChildrenComponent>(ResolvedParent, ChildrenComponent{.Children = {Entity}});
        }
    }
    if (!Name.empty())
        m_Registry.emplace<NameComponent>(Entity, NameComponent{.Name = std::move(Name)});

    m_Registry.emplace<TransformComponent>(Entity);
    return Entity;
}

auto Scene::GetElapsedTime() const -> float {
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - m_StartTime).count();
}

auto Scene::UpdateTime() -> void {
    m_Time = GetElapsedTime();
}

auto Scene::SetAssetRoot(Path AssetRoot) -> void {
    m_AssetRoot = std::move(AssetRoot);
    if (const auto MeshSys = m_SystemScheduler.Get<MeshSystem>())
        MeshSys->get().SetAssetRoot(m_AssetRoot);
}

auto Scene::AddTexturePath(String Path) -> void {
    m_TexturePaths.emplace_back(std::move(Path));
}

auto Scene::GetTexturePaths() const -> const std::vector<String>& {
    return m_TexturePaths;
}

auto Scene::GetRegistry() -> entt::registry& {
    return m_Registry;
}

auto Scene::GetRegistry() const -> const entt::registry& {
    return m_Registry;
}

auto Scene::GetSystems() const -> const SystemScheduler& {
    return m_SystemScheduler;
}

auto Scene::Tick(Float32 DeltaTime) -> void {
    if (const auto Result = m_SystemScheduler.OnUpdate(DeltaTime); !Result)
        LogError("Scene system update failed:\n{}", Result.error().ToString());
    m_SystemScheduler.ClearObservers();
}

auto Scene::CreateEntity(entt::entity Parent) -> entt::entity {
    return CreateEntityInternal({}, Parent);
}

auto Scene::CreateEntityWithName(String Name, entt::entity Parent) -> entt::entity {
    return CreateEntityInternal(std::move(Name), Parent);
}

auto Scene::BuildSnapshot() -> GameSnapshot {
    const auto CameraSys = m_SystemScheduler.Get<CameraSystem>();
    const auto MeshSys   = m_SystemScheduler.Get<MeshSystem>();
    const auto LightSys  = m_SystemScheduler.Get<LightSystem>();
    if (!CameraSys || !MeshSys || !LightSys)
        return GameSnapshot{.Time = m_Time};

    return GameSnapshot{
        .Views     = CameraSys->get().CollectViews(),
        .Instances = CollectSnapshotInstances(),
        .Lights    = LightSys->get().CollectLights(),
        .Textures  = MeshSys->get().GetTextures(),
        .Time      = m_Time,
    };
}

} // namespace SoulEngine
