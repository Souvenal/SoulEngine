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

struct SceneNode {
    String                    Name           = {};
    entt::entity              Parent         = entt::null;
    std::vector<entt::entity> Children       = {};
    Transform                 LocalTransform = {};
    hlslpp::float4x4          WorldTransform = hlslpp::float4x4::identity();
};

/// @brief Physical framebuffer coordinate selected by the editor.
struct RenderPixelCoordinate {
    Uint32 X = 0;
    Uint32 Y = 0;
};

struct SceneSnapshot {
    std::vector<RenderViewSnapshot>      Views          = {};
    std::vector<MeshInfo>                Meshes         = {};
    std::vector<LightSnapshot>           Lights         = {};
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
    UPtr<entt::registry>                                        m_Registry          = nullptr;
    Path                                                        m_AssetRoot         = {};
    std::vector<entt::entity>                                   m_Roots             = {};
    std::map<String, PbrMaterial, std::less<>> m_MaterialInstances = {};
    std::vector<String>                                         m_TexturePaths      = {};
    float                                                       m_Time              = 0.0f;

    [[nodiscard]] auto ResolveAssetPath(StringView AssetPath) const -> String {
        const Path Asset{String(AssetPath)};
        if (Asset.is_absolute() || m_AssetRoot.empty())
            return Asset.lexically_normal().string();
        return (m_AssetRoot / Asset).lexically_normal().string();
    }

  public:
    Scene() : m_Registry(std::make_unique<entt::registry>()) {}
    ~Scene() = default;

    Scene(const Scene&)                    = delete;
    auto operator=(const Scene&) -> Scene& = delete;
    Scene(Scene&&)                         = default;
    auto operator=(Scene&&) -> Scene&      = default;

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
        return *m_Registry;
    }

    [[nodiscard]] auto GetRegistry() const -> const entt::registry& {
        return *m_Registry;
    }

    [[nodiscard]] auto GetRoots() const -> const std::vector<entt::entity>& {
        return m_Roots;
    }

    [[nodiscard]] auto CreateEntity(String Name = {}, entt::entity Parent = entt::null) -> entt::entity {
        const auto Entity = m_Registry->create();
        m_Registry->emplace<SceneNode>(Entity, SceneNode{.Name = std::move(Name), .Parent = Parent});
        if (Parent != entt::null && m_Registry->valid(Parent) && m_Registry->all_of<SceneNode>(Parent))
            m_Registry->get<SceneNode>(Parent).Children.emplace_back(Entity);
        else
            m_Roots.emplace_back(Entity);
        return Entity;
    }

    [[nodiscard]] auto TryGetSceneNode(entt::entity Entity) -> SceneNode* {
        return m_Registry->try_get<SceneNode>(Entity);
    }

    [[nodiscard]] auto TryGetSceneNode(entt::entity Entity) const -> const SceneNode* {
        return m_Registry->try_get<SceneNode>(Entity);
    }

    /// @brief Move relative to the camera's horizontal facing direction and world up.
    /// @brief Rotate from relative cursor movement using yaw and pitch angles.
    ///
    /// Editor navigation is owned by Editor and never mutates a Scene Camera.
    /// These historical prototype API descriptions are retained while the
    /// implementation moves camera navigation out of Scene.

    auto UpdateWorldTransforms() -> void {
        const auto UpdateRecursive =
            [this](auto&& Self, entt::entity Entity, const hlslpp::float4x4& ParentTransform) -> void {
            auto& Node          = m_Registry->get<SceneNode>(Entity);
            Node.WorldTransform = hlslpp::mul(Node.LocalTransform.GetLocalMatrix(), ParentTransform);
            for (const auto Child : Node.Children)
                Self(Self, Child, Node.WorldTransform);
        };

        for (const auto Root : m_Roots) {
            if (m_Registry->valid(Root) && m_Registry->all_of<SceneNode>(Root))
                UpdateRecursive(UpdateRecursive, Root, hlslpp::float4x4::identity());
        }
    }

    [[nodiscard]] auto BuildSnapshot(std::span<const RenderViewSnapshot>  Views          = {},
                                     std::optional<entt::entity>          SelectedEntity = std::nullopt,
                                     std::optional<RenderPixelCoordinate> SelectedPixel  = std::nullopt)
        -> SceneSnapshot {
        UpdateWorldTransforms();
        SceneSnapshot Snapshot{
            .Views          = std::vector<RenderViewSnapshot>(Views.begin(), Views.end()),
            .SelectedEntity = SelectedEntity && m_Registry->valid(*SelectedEntity) ? SelectedEntity : std::nullopt,
            .SelectedPixel  = SelectedPixel,
            .Time           = m_Time,
        };

        const auto Meshes = m_Registry->view<MeshComponent, SceneNode>();
        for (const auto Entity : Meshes) {
            const auto& Mesh = Meshes.get<MeshComponent>(Entity);
            if (Mesh.Asset.empty())
                continue;

            const auto& Node = Meshes.get<SceneNode>(Entity);
            Snapshot.Meshes.emplace_back(MeshInfo{
                .EntityId       = entt::to_integral(Entity),
                .MeshAsset      = ResolveAssetPath(Mesh.Asset),
                .MaterialId     = Mesh.Material,
                .WorldTransform = Node.WorldTransform,
            });
        }

        const auto Lights = m_Registry->view<LightComponent, SceneNode>();
        for (const auto Entity : Lights) {
            const auto& Light        = Lights.get<LightComponent>(Entity);
            const auto& Node         = Lights.get<SceneNode>(Entity);
            const auto  WorldForward = hlslpp::mul(hlslpp::float4(0.0f, 0.0f, -1.0f, 0.0f), Node.WorldTransform);
            const auto  Direction  = hlslpp::normalize(hlslpp::float3(WorldForward.x, WorldForward.y, WorldForward.z));
            const auto  AngleScale = std::numbers::pi_v<Float32> / 180.0f;
            Snapshot.Lights.emplace_back(LightSnapshot{
                .Type      = Light.Type,
                .Color     = hlslpp::float3(Light.ColorR, Light.ColorG, Light.ColorB),
                .Intensity = Light.Intensity,
                .Position =
                    hlslpp::float3(Node.WorldTransform[3].x, Node.WorldTransform[3].y, Node.WorldTransform[3].z),
                .RangeMeters     = Light.RangeMeters,
                .Direction       = Direction,
                .InnerConeCosine = std::cos(Light.InnerConeAngleDegrees * AngleScale),
                .OuterConeCosine = std::cos(Light.OuterConeAngleDegrees * AngleScale),
                .CastsShadows    = Light.CastsShadows,
            });
        }
        return Snapshot;
    }

    [[nodiscard]] auto LoadFromFile(const Path& FilePath) -> std::expected<SceneLoadReport, ErrorMessage>;
};

} // namespace SoulEngine