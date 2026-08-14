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
    String                   Name           = {};
    entt::entity              Parent         = entt::null;
    std::vector<entt::entity> Children       = {};
    Transform                LocalTransform = {};
    hlslpp::float4x4         WorldTransform = hlslpp::float4x4::identity();
};

struct SceneSnapshot {
    std::vector<RenderViewSnapshot> Views           = {};
    std::vector<RenderableInstance> Renderables     = {};
    std::vector<LightSnapshot>      Lights          = {};
    std::optional<entt::entity>      SelectedEntity  = std::nullopt;
    float                           Time            = 0.0f;
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
    std::vector<entt::entity>                                    m_Roots             = {};
    std::map<String, PbrMetallicRoughnessMaterial, std::less<>> m_MaterialInstances = {};
    std::vector<String>                                         m_TexturePaths      = {};
    float                                                       m_Time              = 0.0f;

    [[nodiscard]] auto ResolveAssetPath(StringView AssetPath) const -> String {
        const Path Asset{String(AssetPath)};
        if (Asset.is_absolute() || m_AssetRoot.empty())
            return Asset.lexically_normal().string();
        return (m_AssetRoot / Asset).lexically_normal().string();
    }

  public:
    Scene();
    ~Scene();

    Scene(const Scene&)                    = delete;
    auto operator=(const Scene&) -> Scene& = delete;
    Scene(Scene&&);
    auto operator=(Scene&&) -> Scene&;

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
    auto               SetMaterialInstance(String Id, PbrMetallicRoughnessMaterial Material) -> void;
    [[nodiscard]] auto FindMaterialInstance(StringView Id) const -> const PbrMetallicRoughnessMaterial*;
    [[nodiscard]] auto GetRegistry() -> entt::registry&;
    [[nodiscard]] auto GetRegistry() const -> const entt::registry&;
    [[nodiscard]] auto GetRoots() const -> const std::vector<entt::entity>&;
    [[nodiscard]] auto CreateEntity(String Name = {}, entt::entity Parent = entt::null) -> entt::entity;
    [[nodiscard]] auto TryGetSceneNode(entt::entity Entity) -> SceneNode*;
    [[nodiscard]] auto TryGetSceneNode(entt::entity Entity) const -> const SceneNode*;

    /// @brief Move relative to the camera's horizontal facing direction and world up.
    /// @brief Rotate from relative cursor movement using yaw and pitch angles.
    ///
    /// Editor navigation is owned by Editor and never mutates a Scene Camera.
    /// These historical prototype API descriptions are retained while the
    /// implementation moves camera navigation out of Scene.

    auto UpdateWorldTransforms() -> void;

    [[nodiscard]] auto BuildSnapshot(std::span<const RenderViewSnapshot> Views = {},
                                  std::optional<entt::entity> SelectedEntity = std::nullopt) -> SceneSnapshot;

    [[nodiscard]] auto LoadFromFile(const Path& FilePath) -> std::expected<SceneLoadReport, ErrorMessage>;
};

} // namespace SoulEngine

namespace SoulEngine {

// Keep these definitions inline: MSVC/Xmake emits LNK2005 duplicates when they are non-inline
// in Scene's primary module interface. The implementation-boundary TODO above should remove this.
inline Scene::Scene() : m_Registry(std::make_unique<entt::registry>()) {}
inline Scene::~Scene()                          = default;
inline Scene::Scene(Scene&&)                    = default;
inline auto Scene::operator=(Scene&&) -> Scene& = default;

auto Scene::SetMaterialInstance(String Id, PbrMetallicRoughnessMaterial Material) -> void {
    m_MaterialInstances.insert_or_assign(std::move(Id), Material);
}

[[nodiscard]] auto Scene::FindMaterialInstance(StringView Id) const -> const PbrMetallicRoughnessMaterial* {
    const auto It = m_MaterialInstances.find(String(Id));
    if (It == m_MaterialInstances.end())
        return nullptr;
    return &It->second;
}

[[nodiscard]] auto Scene::GetRegistry() -> entt::registry& {
    return *m_Registry;
}

[[nodiscard]] auto Scene::GetRegistry() const -> const entt::registry& {
    return *m_Registry;
}

[[nodiscard]] auto Scene::GetRoots() const -> const std::vector<entt::entity>& {
    return m_Roots;
}

[[nodiscard]] auto Scene::CreateEntity(String Name, entt::entity Parent) -> entt::entity {
    const auto Entity = m_Registry->create();
    m_Registry->emplace<SceneNode>(Entity, SceneNode{.Name = std::move(Name), .Parent = Parent});
    if (Parent != entt::null && m_Registry->valid(Parent) && m_Registry->all_of<SceneNode>(Parent)) {
        m_Registry->get<SceneNode>(Parent).Children.emplace_back(Entity);
    } else {
        m_Roots.emplace_back(Entity);
    }
    return Entity;
}

[[nodiscard]] auto Scene::TryGetSceneNode(entt::entity Entity) -> SceneNode* {
    return m_Registry->try_get<SceneNode>(Entity);
}

[[nodiscard]] auto Scene::TryGetSceneNode(entt::entity Entity) const -> const SceneNode* {
    return m_Registry->try_get<SceneNode>(Entity);
}

namespace {

auto UpdateWorldTransformRecursive(entt::registry&         Registry,
                                   entt::entity             Entity,
                                   const hlslpp::float4x4& ParentTransform) -> void {
    auto& Node           = Registry.get<SceneNode>(Entity);
    Node.WorldTransform  = hlslpp::mul(Node.LocalTransform.GetLocalMatrix(), ParentTransform);
    for (const auto Child : Node.Children)
        UpdateWorldTransformRecursive(Registry, Child, Node.WorldTransform);
}

} // namespace

auto Scene::UpdateWorldTransforms() -> void {
    for (const auto Root : m_Roots) {
        if (m_Registry->valid(Root) && m_Registry->all_of<SceneNode>(Root))
            UpdateWorldTransformRecursive(*m_Registry, Root, hlslpp::float4x4::identity());
    }
}

[[nodiscard]] auto Scene::BuildSnapshot(std::span<const RenderViewSnapshot> Views,
                          std::optional<entt::entity> SelectedEntity) -> SceneSnapshot {
    UpdateWorldTransforms();
    SceneSnapshot Snapshot{
        .Views          = std::vector<RenderViewSnapshot>(Views.begin(), Views.end()),
        .SelectedEntity = SelectedEntity && m_Registry->valid(*SelectedEntity) ? SelectedEntity : std::nullopt,
        .Time            = m_Time,
    };

    const auto Meshes = m_Registry->view<MeshComponent, SceneNode>();
    for (const auto Entity : Meshes) {
        const auto& Mesh = Meshes.get<MeshComponent>(Entity);
        if (Mesh.Asset.empty())
            continue;

        PbrMetallicRoughnessMaterial Material = {};
        if (!Mesh.Material.empty()) {
            const auto* MaterialInstance = FindMaterialInstance(Mesh.Material);
            if (!MaterialInstance)
                continue;
            Material = *MaterialInstance;
        }

        const auto& Node                = Meshes.get<SceneNode>(Entity);
        const auto  ResolveTextureAsset = [this](String& Texture) -> void {
            if (!Texture.empty())
                Texture = ResolveAssetPath(Texture);
        };
        ResolveTextureAsset(Material.BaseColorTexture);
        ResolveTextureAsset(Material.NormalTexture);
        ResolveTextureAsset(Material.MetallicRoughnessTexture);
        ResolveTextureAsset(Material.MetallicTexture);
        ResolveTextureAsset(Material.RoughnessTexture);
        ResolveTextureAsset(Material.OcclusionTexture);
        ResolveTextureAsset(Material.EmissiveTexture);

        Snapshot.Renderables.emplace_back(RenderableInstance{
            .Entity         = Entity,
            .MeshAsset      = ResolveAssetPath(Mesh.Asset),
            .MaterialId     = Mesh.Material,
            .Material       = std::move(Material),
            .WorldTransform = Node.WorldTransform,
        });
    }
    const auto Lights = m_Registry->view<LightComponent, SceneNode>();
    for (const auto Entity : Lights) {
        const auto& Light = Lights.get<LightComponent>(Entity);
        const auto& Node  = Lights.get<SceneNode>(Entity);
        const auto WorldForward = hlslpp::mul(hlslpp::float4(0.0f, 0.0f, -1.0f, 0.0f), Node.WorldTransform);
        const auto Direction = hlslpp::normalize(hlslpp::float3(WorldForward.x, WorldForward.y, WorldForward.z));
        const auto AngleScale = std::numbers::pi_v<Float32> / 180.0f;
        Snapshot.Lights.emplace_back(LightSnapshot{
            .Type            = Light.Type,
            .Color           = hlslpp::float3(Light.ColorR, Light.ColorG, Light.ColorB),
            .Intensity       = Light.Intensity,
            .Position        = hlslpp::float3(
                Node.WorldTransform[3].x, Node.WorldTransform[3].y, Node.WorldTransform[3].z),
            .RangeMeters     = Light.RangeMeters,
            .Direction       = Direction,
            .InnerConeCosine = std::cos(Light.InnerConeAngleDegrees * AngleScale),
            .OuterConeCosine = std::cos(Light.OuterConeAngleDegrees * AngleScale),
            .CastsShadows    = Light.CastsShadows,
        });
    }
    return Snapshot;
}

} // namespace SoulEngine
