module;

#include <entt/entt.hpp>
#include <hlsl++.h>

export module Scene;

export import Core;
export import Material;
export import RHI;
export import :Camera;
export import :Mesh;
export import :Light;
import TaskGraph;
// export import std;

export namespace SoulEngine {

/// @brief Physical framebuffer coordinate selected by the editor.
struct RenderPixelCoordinate {
    Uint32 X = 0;
    Uint32 Y = 0;
};

struct SceneSnapshot {
    std::vector<RenderViewSnapshot>      Views          = {};
    std::vector<MeshInfo>                Meshes         = {};
    std::vector<LightInfo>               Lights         = {};
    std::optional<entt::entity>          SelectedEntity = std::nullopt;
    std::optional<RenderPixelCoordinate> SelectedPixel  = std::nullopt;
    float                                Time           = 0.0f;
};

struct ComponentWarning {
    String Path    = {};
    String Message = {};
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
    std::chrono::steady_clock::time_point                       m_StartTime         = std::chrono::steady_clock::now();
    // TODO(SoulEngine): Hide EnTT behind a Scene implementation boundary. This should remove
    // both the downstream <entt/entt.hpp> includes required by Scene lifetime instantiation and
    // the inline lifetime definitions kept below for the current MSVC/Xmake module workaround.
    entt::registry                                              m_Registry          = {};
    SystemScheduler                                             m_SystemScheduler;
    Path                                                        m_AssetRoot         = {};
    entt::entity                                                m_RootEntity        = entt::null;
    std::map<String, PbrMaterial, std::less<>> m_MaterialInstances = {};
    std::vector<String>                                         m_TexturePaths      = {};
    float                                                       m_Time              = 0.0f;

    [[nodiscard]] auto ResolveAssetPath(StringView AssetPath) const -> String {
        const Path Asset{String(AssetPath)};
        if (Asset.is_absolute() || m_AssetRoot.empty())
            return Asset.lexically_normal().string();
        return (m_AssetRoot / Asset).lexically_normal().string();
    }

    [[nodiscard]] auto CreateEntityInternal(String Name, entt::entity Parent) -> entt::entity {
        const auto Entity         = m_Registry.create();
        const auto ResolvedParent = Parent != entt::null && m_Registry.valid(Parent) ? Parent : m_RootEntity;

        m_Registry.emplace<ParentComponent>(Entity, ResolvedParent);
        if (ResolvedParent != m_RootEntity) {
            if (m_Registry.all_of<ChildrenComponent>(ResolvedParent)) {
                m_Registry.get<ChildrenComponent>(ResolvedParent).Children.emplace_back(Entity);
            } else {
                m_Registry.emplace<ChildrenComponent>(
                    ResolvedParent, ChildrenComponent{.Children = {Entity}});
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
        m_SystemScheduler.Register<TransformSystem>();
        m_SystemScheduler.Register<MeshSystem>();
        m_SystemScheduler.Register<LightSystem>();
        m_SystemScheduler.Register<CameraSystem>();
        m_SystemScheduler.SetupObservers();
    }
    ~Scene() {
        m_SystemScheduler.TeardownObservers();
    }

    Scene(const Scene&)                    = delete;
    auto operator=(const Scene&) -> Scene& = delete;
    Scene(Scene&&)                         = delete;
    auto operator=(Scene&&) -> Scene& = delete;

    [[nodiscard]] auto GetElapsedTime() const -> float {
        return std::chrono::duration<float>(std::chrono::steady_clock::now() - m_StartTime).count();
    }

    auto UpdateTime() -> void {
        m_Time = GetElapsedTime();
    }

    /// @brief Register a texture asset path. Application calls this during setup.
    auto AddTexturePath(String Path) -> void {
        m_TexturePaths.emplace_back(std::move(Path));
    }

    /// @brief All registered texture paths.
    [[nodiscard]] auto GetTexturePaths() const -> const std::vector<String>& {
        return m_TexturePaths;
    }

    /// @brief Add or replace a scene-local PBR material instance.
    auto SetMaterialInstance(String Id, PbrMaterial Material) -> void {
        m_MaterialInstances.insert_or_assign(std::move(Id), std::move(Material));
    }

    [[nodiscard]] auto FindMaterialInstance(StringView Id) const -> const PbrMaterial* {
        const auto It = m_MaterialInstances.find(String(Id));
        if (It == m_MaterialInstances.end())
            return nullptr;
        return &It->second;
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

    /// @brief Advance scene systems by one frame.
    /// @param DeltaTime Time elapsed since the previous frame in seconds.
    auto Tick(Float32 DeltaTime) -> void {
        m_SystemScheduler.OnUpdate(DeltaTime);
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

    /// @brief Build a complete scene snapshot using system collect methods.
    /// @param SelectedEntity Optional selected entity for editor.
    /// @param SelectedPixel Optional selected pixel coordinate for editor.
    /// @return Complete scene snapshot.
    [[nodiscard]] auto BuildSnapshot(std::optional<entt::entity>          SelectedEntity = std::nullopt,
                                     std::optional<RenderPixelCoordinate> SelectedPixel  = std::nullopt)
        -> SceneSnapshot {
        const auto* CameraSys = m_SystemScheduler.Get<CameraSystem>();
        const auto* MeshSys   = m_SystemScheduler.Get<MeshSystem>();
        const auto* LightSys  = m_SystemScheduler.Get<LightSystem>();
        if (!CameraSys || !MeshSys || !LightSys)
            return SceneSnapshot{.SelectedEntity = std::nullopt, .SelectedPixel = SelectedPixel, .Time = m_Time};

        return SceneSnapshot{
            .Views          = CameraSys->CollectViews(),
            .Meshes         = MeshSys->CollectMeshes(),
            .Lights         = LightSys->CollectLights(),
            .SelectedEntity = SelectedEntity && m_Registry.valid(*SelectedEntity) ? SelectedEntity : std::nullopt,
            .SelectedPixel  = SelectedPixel,
            .Time           = m_Time,
        };
    }

    /// @brief Build a snapshot using explicitly supplied render views.
    /// @param Views Render views to place in the snapshot.
    /// @param SelectedEntity Optional selected entity for editor.
    /// @param SelectedPixel Optional selected pixel coordinate for editor.
    /// @return Complete scene snapshot with system-collected mesh and light data.
    [[nodiscard]] auto BuildSnapshot(std::span<const RenderViewSnapshot> Views,
                                     std::optional<entt::entity>          SelectedEntity = std::nullopt,
                                     std::optional<RenderPixelCoordinate> SelectedPixel  = std::nullopt)
        -> SceneSnapshot {
        const auto* MeshSys  = m_SystemScheduler.Get<MeshSystem>();
        const auto* LightSys = m_SystemScheduler.Get<LightSystem>();
        if (!MeshSys || !LightSys)
            return SceneSnapshot{.Views = {Views.begin(), Views.end()},
                                 .SelectedEntity = std::nullopt,
                                 .SelectedPixel = SelectedPixel,
                                 .Time = m_Time};

        return SceneSnapshot{
            .Views          = {Views.begin(), Views.end()},
            .Meshes         = MeshSys->CollectMeshes(),
            .Lights         = LightSys->CollectLights(),
            .SelectedEntity = SelectedEntity && m_Registry.valid(*SelectedEntity) ? SelectedEntity : std::nullopt,
            .SelectedPixel  = SelectedPixel,
            .Time           = m_Time,
        };
    }

    /// @brief Load a complete Scene from a document file.
    /// @param FilePath Path to the Scene document.
    /// @return An owning Scene and load report, or an error.
    [[nodiscard]] static auto LoadFromFile(const Path& FilePath)
        -> std::expected<std::pair<UPtr<Scene>, SceneLoadReport>, ErrorMessage>;
};

} // namespace SoulEngine
