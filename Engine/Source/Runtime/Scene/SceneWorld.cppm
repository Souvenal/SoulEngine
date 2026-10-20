module;

// Clang 22 named-modules bug: concepts are shadowed through the std re-export chain (import Core/RHI);
// textual include restores them.
#include <concepts>
#include <entt/entt.hpp>
#include <hlsl++.h>

export module Scene:World;

import std;
import Core;
import RHI;
import :Camera;
import :Mesh;
import :Light;
import TaskGraph;

namespace SoulEngine {

export struct GameSnapshot {
    std::vector<CameraViewRecord>   Views     = {};
    std::vector<InstanceRecord>     Instances = {};
    std::vector<LightRecord>        Lights    = {};
    std::vector<RHIRef<RHITexture>> Textures  = {};
    float                           Time      = 0.0f;
};

export struct ComponentWarning {
    String Location = {};
    String Message  = {};
};

export struct SceneLoadReport {
    std::vector<ComponentWarning> Warnings = {};
};

/// @brief World state container read by renderers each frame.
///
/// Holds the camera and (future) mesh, material, light, and transform collections.
export class Scene {
  private:
    std::chrono::steady_clock::time_point m_StartTime = std::chrono::steady_clock::now();
    // TODO(SoulEngine): Hide EnTT behind a Scene implementation boundary. This should remove both the
    // downstream <entt/entt.hpp> includes required by Scene lifetime instantiation and the inline lifetime
    // definitions kept below for the current MSVC/Xmake module workaround.
    entt::registry                        m_Registry  = {};
    SystemScheduler                       m_SystemScheduler;
    Path                                  m_AssetRoot    = {};
    entt::entity                          m_RootEntity   = entt::null;
    std::vector<String>                   m_TexturePaths = {};
    float                                 m_Time         = 0.0f;

    [[nodiscard]] auto ResolveAssetPath(StringView AssetPath) const -> String;

    [[nodiscard]] auto CollectSnapshotInstances() const -> std::vector<InstanceRecord>;

    [[nodiscard]] auto CreateEntityInternal(String Name, entt::entity Parent) -> entt::entity;

  public:
    Scene() : m_SystemScheduler(m_Registry) {
        m_RootEntity = m_Registry.create();
        m_Registry.emplace<TransformComponent>(m_RootEntity);
        m_Registry.ctx().emplace<entt::dispatcher>();
        const auto Setup =
            m_SystemScheduler.Register<TransformSystem>("TransformSystem")
                .and_then([&]() -> std::expected<void, ErrorMessage> {
                    return m_SystemScheduler.Register<MeshSystem>("MeshSystem");
                })
                .and_then([&]() -> std::expected<void, ErrorMessage> {
                    return m_SystemScheduler.Register<LightSystem>("LightSystem");
                })
                .and_then([&]() -> std::expected<void, ErrorMessage> {
                    return m_SystemScheduler.Register<CameraSystem>("CameraSystem");
                })
                .and_then([&]() -> std::expected<void, ErrorMessage> { return m_SystemScheduler.CompileDependency(); });
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

    [[nodiscard]] auto GetElapsedTime() const -> float;

    auto UpdateTime() -> void;

    auto SetAssetRoot(Path AssetRoot) -> void;

    /// @brief Return this Scene's Assets root directory.
    [[nodiscard]] auto GetAssetRoot() const -> const Path& {
        return m_AssetRoot;
    }

    /// @brief Register a texture asset path. Application calls this during setup.
    auto AddTexturePath(String Path) -> void;

    /// @brief All registered texture paths.
    [[nodiscard]] auto GetTexturePaths() const -> const std::vector<String>&;

    [[nodiscard]] auto GetRegistry() -> entt::registry&;

    [[nodiscard]] auto GetRegistry() const -> const entt::registry&;

    /// @brief Access the systems registered for this scene.
    [[nodiscard]] auto GetSystems() const -> const SystemScheduler&;

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
    auto Tick(Float32 DeltaTime) -> void;

    /// @brief Create an unnamed entity under the hidden root or a supplied parent.
    /// @param Parent Parent entity, or entt::null to use the hidden root.
    /// @return The created entity.
    [[nodiscard]] auto CreateEntity(entt::entity Parent = entt::null) -> entt::entity;

    /// @brief Create a named entity under the hidden root or a supplied parent.
    /// @param Name Name assigned to the entity.
    /// @param Parent Parent entity, or entt::null to use the hidden root.
    /// @return The created entity.
    [[nodiscard]] auto CreateEntityWithName(String Name, entt::entity Parent = entt::null) -> entt::entity;

    /// @brief Build a complete game snapshot using system collect methods.
    /// @return Complete game snapshot.
    [[nodiscard]] auto BuildSnapshot() -> GameSnapshot;

    /// @brief Load a complete Scene from a document file.
    /// @param FilePath Path to the Scene document.
    /// @return An owning Scene and load report, or an error.
    [[nodiscard]] static auto LoadFromFile(const Path& FilePath)
        -> std::expected<std::pair<UPtr<Scene>, SceneLoadReport>, ErrorMessage>;
};

} // namespace SoulEngine
