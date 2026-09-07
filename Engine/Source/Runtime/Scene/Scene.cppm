module;

#include <entt/entt.hpp>
#include <hlsl++.h>

export module Scene;

import std;
export import Material;
export import RHI;
export import :Camera;
export import :Mesh;
export import :Light;
import TaskGraph;

export namespace SoulEngine {

struct GameSnapshot {
    std::vector<CameraViewRecord>  Views     = {};
    std::vector<InstanceRecord>    Instances = {};
    std::vector<LightRecord>       Lights    = {};
    RHIRefArray<RHISampledTexture> Textures  = {};
    float                          Time      = 0.0f;
};

struct ComponentWarning {
    String Location = {};
    String Message  = {};
};

struct SceneLoadReport {
    std::vector<ComponentWarning> Warnings = {};
};

/// @brief World state container read by renderers each frame.
///
/// Holds the camera and (future) mesh, material, light, and transform
/// collections.
class Scene {
  private:
    std::chrono::steady_clock::time_point      m_StartTime = std::chrono::steady_clock::now();
    // TODO(SoulEngine): Hide EnTT behind a Scene implementation boundary. This should remove
    // both the downstream <entt/entt.hpp> includes required by Scene lifetime instantiation and
    // the inline lifetime definitions kept below for the current MSVC/Xmake module workaround.
    entt::registry                             m_Registry  = {};
    SystemScheduler                            m_SystemScheduler;
    Path                                       m_AssetRoot         = {};
    entt::entity                               m_RootEntity        = entt::null;
    std::vector<String>                        m_TexturePaths      = {};
    float                                      m_Time              = 0.0f;

    [[nodiscard]] auto ResolveAssetPath(StringView AssetPath) const -> String {
        const Path Asset{String(AssetPath)};
        if (Asset.is_absolute() || m_AssetRoot.empty())
            return Asset.lexically_normal().string();
        return (m_AssetRoot / Asset).lexically_normal().string();
    }

    [[nodiscard]] auto CollectSnapshotInstances() const -> std::vector<InstanceRecord> {
        const auto MeshSys = m_SystemScheduler.Get<MeshSystem>();
        if (!MeshSys)
            return {};

        return MeshSys->get().CollectInstances();
    }

    [[nodiscard]] auto CreateEntityInternal(String Name, entt::entity Parent) -> entt::entity {
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

  public:
    Scene() : m_SystemScheduler(m_Registry) {
        m_RootEntity = m_Registry.create();
        m_Registry.emplace<TransformComponent>(m_RootEntity);
        m_Registry.ctx().emplace<entt::dispatcher>();
        const auto Setup = m_SystemScheduler.Register<TransformSystem>("TransformSystem")
                               .and_then([&]() -> std::expected<void, ErrorMessage> {
                                   return m_SystemScheduler.Register<MeshSystem>("MeshSystem");
                               })
                               .and_then([&]() -> std::expected<void, ErrorMessage> {
                                   return m_SystemScheduler.Register<LightSystem>("LightSystem");
                               })
                               .and_then([&]() -> std::expected<void, ErrorMessage> {
                                   return m_SystemScheduler.Register<CameraSystem>("CameraSystem");
                               })
                               .and_then([&]() -> std::expected<void, ErrorMessage> {
                                   return m_SystemScheduler.CompileDependency();
                               });
        if (!Setup)
            LogError("Scene system setup failed:\n{}", Setup.error().ToString());
        m_SystemScheduler.SetupObservers();
    }
    ~Scene() {
        m_SystemScheduler.TeardownObservers();
    }

    Scene(const Scene&)                    = delete;
    auto operator=(const Scene&) -> Scene& = delete;
    Scene(Scene&&)                         = delete;
    auto operator=(Scene&&) -> Scene&      = delete;

    [[nodiscard]] auto GetElapsedTime() const -> float {
        return std::chrono::duration<float>(std::chrono::steady_clock::now() - m_StartTime).count();
    }

    auto UpdateTime() -> void {
        m_Time = GetElapsedTime();
    }

    auto SetAssetRoot(Path AssetRoot) -> void {
        m_AssetRoot = std::move(AssetRoot);
        if (const auto MeshSys = m_SystemScheduler.Get<MeshSystem>())
            MeshSys->get().SetAssetRoot(m_AssetRoot);
    }

    /// @brief Return this Scene's Assets root directory.
    [[nodiscard]] auto GetAssetRoot() const -> const Path& {
        return m_AssetRoot;
    }

    /// @brief Register a texture asset path. Application calls this during setup.
    auto AddTexturePath(String Path) -> void {
        m_TexturePaths.emplace_back(std::move(Path));
    }

    /// @brief All registered texture paths.
    [[nodiscard]] auto GetTexturePaths() const -> const std::vector<String>& {
        return m_TexturePaths;
    }

    [[nodiscard]] auto GetRegistry() -> entt::registry& {
        return m_Registry;
    }

    [[nodiscard]] auto GetRegistry() const -> const entt::registry& {
        return m_Registry;
    }

    /// @brief Access the systems registered for this scene.
    [[nodiscard]] auto GetSystems() const -> const SystemScheduler& {
        return m_SystemScheduler;
    }

    /// @brief Get a registered Scene system for read-only inspection.
    /// @tparam T System implementation derived from ISystem.
    /// @return A borrowed const system reference, or nullopt if T is not registered.
    template <typename T>
        requires std::derived_from<T, ISystem>
    [[nodiscard]] auto GetSystem() const -> std::optional<std::reference_wrapper<const T>> {
        return m_SystemScheduler.Get<T>();
    }

    /// @brief Advance scene systems by one frame.
    /// @param DeltaTime Time elapsed since the previous frame in seconds.
    auto Tick(Float32 DeltaTime) -> void {
        if (const auto Result = m_SystemScheduler.OnUpdate(DeltaTime); !Result)
            LogError("Scene system update failed:\n{}", Result.error().ToString());
        m_SystemScheduler.ClearObservers();
    }

    /// @brief Create an unnamed entity under the hidden root or a supplied parent.
    /// @param Parent Parent entity, or entt::null to use the hidden root.
    /// @return The created entity.
    [[nodiscard]] auto CreateEntity(entt::entity Parent = entt::null) -> entt::entity {
        return CreateEntityInternal({}, Parent);
    }

    /// @brief Create a named entity under the hidden root or a supplied parent.
    /// @param Name Name assigned to the entity.
    /// @param Parent Parent entity, or entt::null to use the hidden root.
    /// @return The created entity.
    [[nodiscard]] auto CreateEntityWithName(String Name, entt::entity Parent = entt::null) -> entt::entity {
        return CreateEntityInternal(std::move(Name), Parent);
    }

    /// @brief Build a complete game snapshot using system collect methods.
    /// @return Complete game snapshot.
    [[nodiscard]] auto BuildSnapshot() -> GameSnapshot {
        const auto CameraSys = m_SystemScheduler.Get<CameraSystem>();
        const auto MeshSys   = m_SystemScheduler.Get<MeshSystem>();
        const auto LightSys  = m_SystemScheduler.Get<LightSystem>();
        if (!CameraSys || !MeshSys || !LightSys)
            return GameSnapshot{.Time = m_Time};

        return GameSnapshot{
            .Views     = CameraSys->get().CollectViews(),
            .Instances = CollectSnapshotInstances(),
            .Lights    = LightSys->get().CollectLights(),
            .Textures  = MeshSys->get().GetTextureArray(),
            .Time      = m_Time,
        };
    }

    /// @brief Load a complete Scene from a document file.
    /// @param FilePath Path to the Scene document.
    /// @return An owning Scene and load report, or an error.
    [[nodiscard]] static auto LoadFromFile(const Path& FilePath)
        -> std::expected<std::pair<UPtr<Scene>, SceneLoadReport>, ErrorMessage>;
};

} // namespace SoulEngine
