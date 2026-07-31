module;

#include <entt/entt.hpp>
#include <hlsl++.h>

export module Scene;

export import Core;
export import Material;
export import RHI;
import TaskGraph;
// export import std;

export namespace SoulEngine {

using SceneEntity = entt::entity;

struct Transform {
    hlslpp::float3   Translation    = hlslpp::float3(0.0f, 0.0f, 0.0f);
    hlslpp::float3   Rotation       = hlslpp::float3(0.0f, 0.0f, 0.0f);
    hlslpp::float3   Scale          = hlslpp::float3(1.0f, 1.0f, 1.0f);
    hlslpp::float4x4 WorldTransform = hlslpp::float4x4::identity();

    [[nodiscard]] auto GetLocalMatrix() const -> hlslpp::float4x4 {
        const auto RotationRadians = Rotation * (std::numbers::pi_v<float> / 180.0f);
        return hlslpp::mul(hlslpp::mul(hlslpp::mul(hlslpp::mul(hlslpp::float4x4::scale(Scale),
                                                               hlslpp::float4x4::rotation_x(RotationRadians.x)),
                                                   hlslpp::float4x4::rotation_y(RotationRadians.y)),
                                       hlslpp::float4x4::rotation_z(RotationRadians.z)),
                           hlslpp::float4x4::translation(Translation));
    }
};

/// @brief Immutable render data and resources for one camera/view.
struct RenderViewSnapshot {
    hlslpp::float4x4        ViewProjection = hlslpp::float4x4::identity();
    hlslpp::float3          CameraPosition = hlslpp::float3(0.0f, 0.0f, 0.0f);
    RHIRef<RHIRenderTarget> ColorRT        = nullptr;
    RHIRef<RHIRenderTarget> DepthRT        = nullptr;
};

struct SceneNode {
    String                   Name      = {};
    SceneEntity              Parent    = entt::null;
    std::vector<SceneEntity> Children  = {};
    Transform                Transform = {};
};

/// @brief Simple camera holding world-space position and orientation.
///
/// Minimal prototype — projection parameters are included so renderers can
/// derive view and projection matrices without depending on math headers.
// TODO: Split Camera into specialized camera types when their ownership and
// projection policies become concrete. Expected variants include gameplay
// cameras, editor viewport cameras, and shadow cameras. Keep this base type
// minimal for now: view parameters plus view-scoped resource refs only.
/// @brief Reusable lens and view-matrix policy for scene and editor cameras.
struct Camera {
    float                   FOV            = 60.0f;
    float                   NearPlane      = 0.1f;
    float                   FarPlane       = 100.0f;
    RHIRef<RHIRenderTarget> ColorRT        = nullptr;
    RHIRef<RHIRenderTarget> DepthRT        = nullptr;
    Uint32                  ViewportWidth  = 0;
    Uint32                  ViewportHeight = 0;

    /// @brief Resize the camera-owned output resources.
    auto ResizeViewport(StringView ResourceKey, Uint32 Width, Uint32 Height) -> void {
        if (Width == 0 || Height == 0) {
            ColorRT        = nullptr;
            DepthRT        = nullptr;
            ViewportWidth  = 0;
            ViewportHeight = 0;
            return;
        }

        if (ViewportWidth == Width && ViewportHeight == Height && ColorRT && DepthRT)
            return;

        ViewportWidth  = Width;
        ViewportHeight = Height;
        const RHIRenderTargetDesc ColorDesc{
            .Width  = Width,
            .Height = Height,
            .Format = RHIFormat::B8G8R8A8_UNORM,
            .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::FrameOutput,
        };
        const RHIRenderTargetDesc DepthDesc{
            .Width  = Width,
            .Height = Height,
            .Format = RHIFormat::D32_SFLOAT,
            .Usage  = RHITextureUsage::DepthStencil,
        };
        auto Color = RHIRenderDevice::Get().CreateRenderTarget(ColorDesc);
        if (!Color) {
            LogError("Failed to queue camera color render target creation: {}", Color.error().ToString());
            ColorRT = nullptr;
        } else {
            ColorRT = std::move(*Color);
        }

        auto Depth = RHIRenderDevice::Get().CreateRenderTarget(DepthDesc);
        if (!Depth) {
            LogError("Failed to queue camera depth render target creation: {}", Depth.error().ToString());
            DepthRT = nullptr;
        } else {
            DepthRT = std::move(*Depth);
        }
    }

    /// Vulkan projection: right-handed, zclip [0,1], forward depth, finite far plane.
    [[nodiscard]] auto GetProjectionMatrix(float AspectRatio) const -> hlslpp::float4x4 {
        const float FovRad = FOV * (std::numbers::pi_v<float> / 180.0f);
        return hlslpp::float4x4::perspective(
            hlslpp::projection(hlslpp::frustum::field_of_view_y(FovRad, AspectRatio, NearPlane, FarPlane),
                               hlslpp::zclip::zero,
                               hlslpp::zdirection::forward,
                               hlslpp::zplane::finite));
    }

    [[nodiscard]] auto GetForward(const Transform& CameraTransform) const -> hlslpp::float3 {
        const auto LocalForward = hlslpp::float4(0.0f, 0.0f, -1.0f, 0.0f);
        const auto WorldForward = hlslpp::mul(LocalForward, CameraTransform.WorldTransform);
        return hlslpp::normalize(hlslpp::float3(WorldForward.x, WorldForward.y, WorldForward.z));
    }

    [[nodiscard]] auto GetViewMatrix(const Transform& CameraTransform) const -> hlslpp::float4x4 {
        const auto& World    = CameraTransform.WorldTransform;
        const auto  Position = hlslpp::float3(World[3].x, World[3].y, World[3].z);
        return hlslpp::float4x4::look_at(
            Position, Position + GetForward(CameraTransform), hlslpp::float3(0.0f, 1.0f, 0.0f));
    }

    [[nodiscard]] auto BuildRenderView(const Transform& CameraTransform) const -> std::optional<RenderViewSnapshot> {
        if (!ColorRT || !DepthRT || ViewportWidth == 0 || ViewportHeight == 0)
            return std::nullopt;

        auto ColorRTRef = ColorRT;
        auto DepthRTRef = DepthRT;
        if (!ColorRTRef.TryGet() || !DepthRTRef.TryGet())
            return std::nullopt;

        const float AspectRatio = static_cast<float>(ViewportWidth) / static_cast<float>(ViewportHeight);
        const auto& World       = CameraTransform.WorldTransform;
        return RenderViewSnapshot{
            .ViewProjection = hlslpp::mul(GetViewMatrix(CameraTransform), GetProjectionMatrix(AspectRatio)),
            .CameraPosition = hlslpp::float3(World[3].x, World[3].y, World[3].z),
            .ColorRT        = std::move(ColorRTRef),
            .DepthRT        = std::move(DepthRTRef),
        };
    }
};

// CameraComponent persists only the camera lens parameters. Camera output
// resources are non-persisted runtime state.
struct CameraComponent {
    Camera Settings = {};

    [[nodiscard]] auto GetFOV() const -> float {
        return Settings.FOV;
    }

    auto SetFOV(float Value) -> void {
        Settings.FOV = Value;
    }

    [[nodiscard]] auto GetNearPlane() const -> float {
        return Settings.NearPlane;
    }

    auto SetNearPlane(float Value) -> void {
        Settings.NearPlane = Value;
    }

    [[nodiscard]] auto GetFarPlane() const -> float {
        return Settings.FarPlane;
    }

    auto SetFarPlane(float Value) -> void {
        Settings.FarPlane = Value;
    }
};

/// @brief Scene-authored mesh asset reference.
///
/// Paths are relative to the current application Assets directory. Renderer-specific
/// mesh resources, uploads, and draw representations are
/// owned by each renderer rather than this component.
struct MeshComponent {
    String Asset    = {};
    /// Scene-local PBR material instance ID. Empty uses the built-in material defaults.
    String Material = {};
};

struct LightComponent {};

/// @brief Immutable CPU render record for one mesh asset instance.
///
/// It intentionally carries only renderer-neutral asset identity and instance
/// state. Each renderer resolves it to its own GPU representation.
struct RenderableInstance {
    String                       MeshAsset      = {};
    /// Scene-local material instance ID. Empty identifies the shared built-in material.
    String                       MaterialId     = {};
    PbrMetallicRoughnessMaterial Material       = {};
    hlslpp::float4x4             WorldTransform = hlslpp::float4x4::identity();
};

struct SceneSnapshot {
    std::vector<RenderViewSnapshot> Views       = {};
    std::vector<RenderableInstance> Renderables = {};
    float                           Time        = 0.0f;
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
    std::vector<SceneEntity>                                    m_Roots             = {};
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
    [[nodiscard]] auto GetMaterialInstances() const
        -> const std::map<String, PbrMetallicRoughnessMaterial, std::less<>>&;

    [[nodiscard]] auto GetRegistry() -> entt::registry&;
    [[nodiscard]] auto GetRegistry() const -> const entt::registry&;
    [[nodiscard]] auto GetRoots() const -> const std::vector<SceneEntity>&;
    [[nodiscard]] auto CreateEntity(String Name = {}, SceneEntity Parent = entt::null) -> SceneEntity;
    [[nodiscard]] auto TryGetSceneNode(SceneEntity Entity) -> SceneNode*;
    [[nodiscard]] auto TryGetSceneNode(SceneEntity Entity) const -> const SceneNode*;

    /// @brief Move relative to the camera's horizontal facing direction and world up.
    /// @brief Rotate from relative cursor movement using yaw and pitch angles.
    ///
    /// Editor navigation is owned by Editor and never mutates a Scene Camera.
    /// These historical prototype API descriptions are retained while the
    /// implementation moves camera navigation out of Scene.

    auto UpdateWorldTransforms() -> void;

    [[nodiscard]] auto BuildSnapshot(std::span<const RenderViewSnapshot> Views = {}) -> SceneSnapshot;

    [[nodiscard]] auto LoadFromFile(const Path& FilePath) -> std::expected<SceneLoadReport, ErrorMessage>;
    [[nodiscard]] auto SaveToFile(const Path& FilePath) const -> std::expected<void, ErrorMessage>;
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

[[nodiscard]] auto Scene::GetMaterialInstances() const
    -> const std::map<String, PbrMetallicRoughnessMaterial, std::less<>>& {
    return m_MaterialInstances;
}

[[nodiscard]] auto Scene::GetRegistry() -> entt::registry& {
    return *m_Registry;
}

[[nodiscard]] auto Scene::GetRegistry() const -> const entt::registry& {
    return *m_Registry;
}

[[nodiscard]] auto Scene::GetRoots() const -> const std::vector<SceneEntity>& {
    return m_Roots;
}

[[nodiscard]] auto Scene::CreateEntity(String Name, SceneEntity Parent) -> SceneEntity {
    const auto Entity = m_Registry->create();
    m_Registry->emplace<SceneNode>(Entity, SceneNode{.Name = std::move(Name), .Parent = Parent});
    if (Parent != entt::null && m_Registry->valid(Parent) && m_Registry->all_of<SceneNode>(Parent)) {
        m_Registry->get<SceneNode>(Parent).Children.emplace_back(Entity);
    } else {
        m_Roots.emplace_back(Entity);
    }
    return Entity;
}

[[nodiscard]] auto Scene::TryGetSceneNode(SceneEntity Entity) -> SceneNode* {
    return m_Registry->try_get<SceneNode>(Entity);
}

[[nodiscard]] auto Scene::TryGetSceneNode(SceneEntity Entity) const -> const SceneNode* {
    return m_Registry->try_get<SceneNode>(Entity);
}

namespace {

auto UpdateWorldTransformRecursive(entt::registry&         Registry,
                                   SceneEntity             Entity,
                                   const hlslpp::float4x4& ParentTransform) -> void {
    auto& Node                    = Registry.get<SceneNode>(Entity);
    Node.Transform.WorldTransform = hlslpp::mul(Node.Transform.GetLocalMatrix(), ParentTransform);
    for (const auto Child : Node.Children)
        UpdateWorldTransformRecursive(Registry, Child, Node.Transform.WorldTransform);
}

} // namespace

auto Scene::UpdateWorldTransforms() -> void {
    for (const auto Root : m_Roots) {
        if (m_Registry->valid(Root) && m_Registry->all_of<SceneNode>(Root))
            UpdateWorldTransformRecursive(*m_Registry, Root, hlslpp::float4x4::identity());
    }
}

[[nodiscard]] auto Scene::BuildSnapshot(std::span<const RenderViewSnapshot> Views) -> SceneSnapshot {
    UpdateWorldTransforms();
    SceneSnapshot Snapshot{
        .Views = std::vector<RenderViewSnapshot>(Views.begin(), Views.end()),
        .Time  = m_Time,
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
            .MeshAsset      = ResolveAssetPath(Mesh.Asset),
            .MaterialId     = Mesh.Material,
            .Material       = std::move(Material),
            .WorldTransform = Node.Transform.WorldTransform,
        });
    }
    return Snapshot;
}

} // namespace SoulEngine
