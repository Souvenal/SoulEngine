module;

#include <entt/entt.hpp>
#include <hlsl++.h>

export module Scene;

export import Core;
export import Material;
export import Resource;
// export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Scene {

using SceneEntity = entt::entity;

/// @brief GPU data layout for one render view's constant buffer.
/// Matches Common.slang ViewData std140 layout.
struct alignas(16) ViewConstants {
    alignas(16) hlslpp::float4x4 ViewProjection = hlslpp::float4x4::identity();
};
static_assert(sizeof(ViewConstants) == 64, "ViewConstants must match Common.slang ViewData std140 layout");

struct Transform {
    hlslpp::float3   Translation     = hlslpp::float3(0.0f, 0.0f, 0.0f);
    hlslpp::float3   RotationDegrees = hlslpp::float3(0.0f, 0.0f, 0.0f);
    hlslpp::float3   Scale           = hlslpp::float3(1.0f, 1.0f, 1.0f);
    hlslpp::float4x4 WorldTransform  = hlslpp::float4x4::identity();
};

struct SceneNode {
    String                   Name     = {};
    SceneEntity              Parent   = entt::null;
    std::vector<SceneEntity> Children = {};
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
struct CameraComponent {
    float FOV       = 60.0f;
    float NearPlane = 0.1f;
    float FarPlane  = 100.0f;
    float AspectRatio = 16.0f / 9.0f;
    Resource::ResourceRef<RHI::RenderTarget> ColorRT = {};
    Resource::ResourceRef<RHI::RenderTarget> DepthRT = {};
    /// Must be unique among concurrently rendered cameras.
    String ViewConstantBufferKey = {};
    /// Logical constant buffer owned by this view.
    Resource::ResourceRef<RHI::ConstantBuffer> ViewCB = {};

    /// Vulkan projection: right-handed, zclip [0,1], forward depth, finite far plane.
    [[nodiscard]] auto GetProjectionMatrix() const -> hlslpp::float4x4 {
        const float FovRad = FOV * (std::numbers::pi_v<float> / 180.0f);
        return hlslpp::float4x4::perspective(
            hlslpp::projection(hlslpp::frustum::field_of_view_y(FovRad, AspectRatio, NearPlane, FarPlane),
                               hlslpp::zclip::zero,
                               hlslpp::zdirection::forward,
                               hlslpp::zplane::finite));
    }
};

/// @brief Scene-authored mesh asset reference.
///
/// Paths are relative to the current application Assets directory. Renderer-specific
/// mesh resources, uploads, and draw representations are
/// owned by each renderer rather than this component.
struct MeshComponent {
    String Asset = {};
    /// Scene-local PBR material instance ID. Empty uses the built-in material defaults.
    String Material = {};
    /// Optional base-color texture path relative to the current application Assets directory.
    /// Empty renders with material factors only.
    String Texture = {};
};

struct LightComponent {};

/// @brief Immutable render data and resources for one camera/view.
struct RenderViewSnapshot {
    hlslpp::float4x4                              ViewProjection = hlslpp::float4x4::identity();
    hlslpp::float3                                CameraPosition = hlslpp::float3(0.0f, 0.0f, 0.0f);
    Resource::ResourceHandle<RHI::RenderTarget>   ColorRT        = {};
    Resource::ResourceHandle<RHI::RenderTarget>   DepthRT        = {};
    Resource::ResourceHandle<RHI::ConstantBuffer> ViewCB         = {};

    [[nodiscard]] auto GetViewConstants() const -> ViewConstants {
        return ViewConstants{.ViewProjection = ViewProjection};
    }
};

/// @brief Immutable CPU render record for one mesh asset instance.
///
/// It intentionally carries only renderer-neutral asset identity and instance
/// state. Each renderer resolves it to its own GPU representation.
struct RenderableInstance {
    String                        MeshAsset      = {};
    String                        TextureAsset   = {};
    Material::PbrMetallicRoughnessMaterial  Material       = {};
    hlslpp::float4x4              WorldTransform = hlslpp::float4x4::identity();
};

struct SceneSnapshot {
    std::vector<RenderViewSnapshot>  Views       = {};
    std::vector<RenderableInstance>  Renderables = {};
    float                            Time        = 0.0f;
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
    std::chrono::steady_clock::time_point m_StartTime = std::chrono::steady_clock::now();
    // TODO(SoulEngine): Hide EnTT behind a Scene implementation boundary. This should remove
    // both the downstream <entt/entt.hpp> includes required by Scene lifetime instantiation and
    // the inline lifetime definitions kept below for the current MSVC/Xmake module workaround.
    UPtr<entt::registry>                                         m_Registry = nullptr;
    std::vector<SceneEntity>                                      m_Roots = {};
    std::map<String, Material::PbrMetallicRoughnessMaterial, std::less<>>   m_MaterialInstances = {};
    std::vector<String>                                           m_TexturePaths = {};
    float                                                         m_Time = 0.0f;

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
    auto SetMaterialInstance(String Id, Material::PbrMetallicRoughnessMaterial Material) -> void;
    [[nodiscard]] auto FindMaterialInstance(StringView Id) const -> const Material::PbrMetallicRoughnessMaterial*;
    [[nodiscard]] auto GetMaterialInstances() const
        -> const std::map<String, Material::PbrMetallicRoughnessMaterial, std::less<>>&;

    [[nodiscard]] auto GetRegistry() -> entt::registry&;
    [[nodiscard]] auto GetRegistry() const -> const entt::registry&;
    [[nodiscard]] auto GetRoots() const -> const std::vector<SceneEntity>&;
    [[nodiscard]] auto CreateEntity(String Name = {}, SceneEntity Parent = entt::null) -> SceneEntity;
    [[nodiscard]] auto TryGetSceneNode(SceneEntity Entity) -> SceneNode*;
    [[nodiscard]] auto TryGetSceneNode(SceneEntity Entity) const -> const SceneNode*;

    auto AllocateCameraRenderTargets(Uint32 Width, Uint32 Height) -> void;

    /// @brief Move relative to the camera's horizontal facing direction and world up.
    auto MoveFirstCamera(float ForwardInput, float RightInput, float VerticalInput, float ZoomInput, float DeltaTime)
        -> void;

    /// @brief Rotate from relative cursor movement using yaw and pitch angles.
    auto RotateFirstCamera(float CursorDeltaX, float CursorDeltaY) -> void;

    auto UpdateWorldTransforms() -> void;

    [[nodiscard]] auto BuildSnapshot() -> SceneSnapshot;

    [[nodiscard]] auto LoadFromFile(const Path& FilePath) -> std::expected<SceneLoadReport, ErrorMessage>;
    [[nodiscard]] auto SaveToFile(const Path& FilePath) const -> std::expected<void, ErrorMessage>;
};

} // namespace SoulEngine::Scene

namespace SoulEngine::Scene {

// Keep these definitions inline: MSVC/Xmake emits LNK2005 duplicates when they are non-inline
// in Scene's primary module interface. The implementation-boundary TODO above should remove this.
inline Scene::Scene() : m_Registry(std::make_unique<entt::registry>()) {}
inline Scene::~Scene() = default;
inline Scene::Scene(Scene&&) = default;
inline auto Scene::operator=(Scene&&) -> Scene& = default;

auto Scene::SetMaterialInstance(String Id, Material::PbrMetallicRoughnessMaterial Material) -> void {
    m_MaterialInstances.insert_or_assign(std::move(Id), Material);
}

[[nodiscard]] auto Scene::FindMaterialInstance(StringView Id) const -> const Material::PbrMetallicRoughnessMaterial* {
    const auto It = m_MaterialInstances.find(String(Id));
    if (It == m_MaterialInstances.end())
        return nullptr;
    return &It->second;
}

[[nodiscard]] auto Scene::GetMaterialInstances() const
    -> const std::map<String, Material::PbrMetallicRoughnessMaterial, std::less<>>& {
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

[[nodiscard]] auto MakeLocalTransform(const Transform& InTransform) -> hlslpp::float4x4 {
    const auto RotationRadians = InTransform.RotationDegrees * (std::numbers::pi_v<float> / 180.0f);
    return hlslpp::mul(
        hlslpp::mul(
            hlslpp::mul(
                hlslpp::mul(hlslpp::float4x4::scale(InTransform.Scale),
                             hlslpp::float4x4::rotation_x(RotationRadians.x)),
                hlslpp::float4x4::rotation_y(RotationRadians.y)),
            hlslpp::float4x4::rotation_z(RotationRadians.z)),
        hlslpp::float4x4::translation(InTransform.Translation));
}

auto UpdateWorldTransformRecursive(entt::registry& Registry,
                                   SceneEntity Entity,
                                   const hlslpp::float4x4& ParentTransform) -> void {
    auto& Node = Registry.get<SceneNode>(Entity);
    Node.Transform.WorldTransform = hlslpp::mul(MakeLocalTransform(Node.Transform), ParentTransform);
    for (const auto Child : Node.Children)
        UpdateWorldTransformRecursive(Registry, Child, Node.Transform.WorldTransform);
}

[[nodiscard]] auto GetWorldPosition(const SceneNode& Node) -> hlslpp::float3 {
    const auto& World = Node.Transform.WorldTransform;
    return hlslpp::float3(World[3].x, World[3].y, World[3].z);
}

[[nodiscard]] auto GetWorldForward(const SceneNode& Node) -> hlslpp::float3 {
    const auto LocalForward = hlslpp::float4(0.0f, 0.0f, -1.0f, 0.0f);
    const auto WorldForward = hlslpp::mul(LocalForward, Node.Transform.WorldTransform);
    return hlslpp::normalize(hlslpp::float3(WorldForward.x, WorldForward.y, WorldForward.z));
}

[[nodiscard]] auto GetFirstCameraEntity(const entt::registry& Registry) -> SceneEntity {
    const auto View = Registry.view<CameraComponent, SceneNode>();
    for (const auto Entity : View)
        return Entity;
    return entt::null;
}

} // namespace

auto Scene::AllocateCameraRenderTargets(Uint32 Width, Uint32 Height) -> void {
    const auto Cameras = m_Registry->view<CameraComponent>();
    for (const auto Entity : Cameras) {
        auto& Camera = Cameras.get<CameraComponent>(Entity);
        if (Width == 0 || Height == 0) {
            Camera.ColorRT.Reset();
            Camera.DepthRT.Reset();
            return;
        }

        if (Camera.ViewConstantBufferKey.empty())
            Camera.ViewConstantBufferKey = Format("camera_viewcb_{}", entt::to_integral(Entity));
        if (!Camera.ViewCB)
            Camera.ViewCB = Resource::Manager::Get().RequestConstantBufferRef(
                Camera.ViewConstantBufferKey, {.Size = sizeof(ViewConstants)});

        const auto ColorKey = Format("camera_{}_color_{}x{}", entt::to_integral(Entity), Width, Height);
        const auto DepthKey = Format("camera_{}_depth_{}x{}", entt::to_integral(Entity), Width, Height);
        const auto& ColorHandle = Camera.ColorRT.GetHandle();
        const auto& DepthHandle = Camera.DepthRT.GetHandle();
        if (ColorHandle.IsValid() && ColorHandle.GetKey() == ColorKey && DepthHandle.IsValid() &&
            DepthHandle.GetKey() == DepthKey) {
            Camera.AspectRatio = static_cast<float>(Width) / static_cast<float>(Height);
            continue;
        }

        Camera.ColorRT = Resource::Manager::Get().RequestRenderTargetRef(
            ColorKey,
            RHI::RenderTargetDesc{
                .Width  = Width,
                .Height = Height,
                .Format = RHI::Format::B8G8R8A8_UNORM,
                .Usage  = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::FrameOutput,
            });
        Camera.DepthRT = Resource::Manager::Get().RequestRenderTargetRef(
            DepthKey,
            RHI::RenderTargetDesc{
                .Width  = Width,
                .Height = Height,
                .Format = RHI::Format::D32_SFLOAT,
                .Usage  = RHI::TextureUsage::DepthStencil,
            });
        Camera.AspectRatio = static_cast<float>(Width) / static_cast<float>(Height);
    }
}

auto Scene::MoveFirstCamera(float ForwardInput,
                            float RightInput,
                            float VerticalInput,
                            float ZoomInput,
                            float DeltaTime) -> void {
    UpdateWorldTransforms();
    const auto Entity = GetFirstCameraEntity(*m_Registry);
    if (Entity == entt::null)
        return;

    auto& Node = m_Registry->get<SceneNode>(Entity);
    const auto Forward = GetWorldForward(Node);
    const auto HorizontalForward = hlslpp::normalize(hlslpp::float3(Forward.x, 0.0f, Forward.z));
    const auto Up = hlslpp::float3(0.0f, 1.0f, 0.0f);
    const auto Right = hlslpp::normalize(hlslpp::cross(HorizontalForward, Up));
    auto MoveDirection = HorizontalForward * ForwardInput + Right * RightInput + Up * VerticalInput;
    if (MoveDirection.x != 0.0f || MoveDirection.y != 0.0f || MoveDirection.z != 0.0f)
        Node.Transform.Translation += hlslpp::normalize(MoveDirection) * (2.0f * DeltaTime);

    Node.Transform.Translation += Forward * (ZoomInput * 0.75f);
}

auto Scene::RotateFirstCamera(float CursorDeltaX, float CursorDeltaY) -> void {
    const auto Entity = GetFirstCameraEntity(*m_Registry);
    if (Entity == entt::null)
        return;

    constexpr float Sensitivity = 0.0025f;
    constexpr float MaxPitch    = 1.55334306f;
    auto& Node = m_Registry->get<SceneNode>(Entity);
    auto& Rotation = Node.Transform.RotationDegrees;
    Rotation.y += CursorDeltaX * Sensitivity * (180.0f / std::numbers::pi_v<float>);
    Rotation.x = std::clamp(
        static_cast<float>(Rotation.x) + CursorDeltaY * Sensitivity * (180.0f / std::numbers::pi_v<float>),
        -MaxPitch * (180.0f / std::numbers::pi_v<float>),
        MaxPitch * (180.0f / std::numbers::pi_v<float>));
}

auto Scene::UpdateWorldTransforms() -> void {
    for (const auto Root : m_Roots) {
        if (m_Registry->valid(Root) && m_Registry->all_of<SceneNode>(Root))
            UpdateWorldTransformRecursive(*m_Registry, Root, hlslpp::float4x4::identity());
    }
}

[[nodiscard]] auto Scene::BuildSnapshot() -> SceneSnapshot {
    UpdateWorldTransforms();
    SceneSnapshot Snapshot{
        .Time = m_Time,
    };

    const auto Cameras = m_Registry->view<CameraComponent, SceneNode>();
    for (const auto Entity : Cameras) {
        const auto& Camera = Cameras.get<CameraComponent>(Entity);
        const auto& Node = Cameras.get<SceneNode>(Entity);
        const auto Position = GetWorldPosition(Node);
        const auto Forward = GetWorldForward(Node);
        const auto View = hlslpp::float4x4::look_at(Position, Position + Forward, hlslpp::float3(0.0f, 1.0f, 0.0f));
        Snapshot.Views.emplace_back(RenderViewSnapshot{
            .ViewProjection = hlslpp::mul(View, Camera.GetProjectionMatrix()),
            .CameraPosition = Position,
            .ColorRT        = Camera.ColorRT.GetHandle(),
            .DepthRT        = Camera.DepthRT.GetHandle(),
            .ViewCB         = Camera.ViewCB.GetHandle(),
        });
    }

    const auto Meshes = m_Registry->view<MeshComponent, SceneNode>();
    for (const auto Entity : Meshes) {
        const auto& Mesh = Meshes.get<MeshComponent>(Entity);
        if (Mesh.Asset.empty())
            continue;

        Material::PbrMetallicRoughnessMaterial Material = {};
        if (!Mesh.Material.empty()) {
            const auto* MaterialInstance = FindMaterialInstance(Mesh.Material);
            if (!MaterialInstance)
                continue;
            Material = *MaterialInstance;
        }

        const auto& Node = Meshes.get<SceneNode>(Entity);
        Snapshot.Renderables.emplace_back(RenderableInstance{
            .MeshAsset      = Mesh.Asset,
            .TextureAsset   = Mesh.Texture,
            .Material       = Material,
            .WorldTransform = Node.Transform.WorldTransform,
        });
    }
    return Snapshot;
}

} // namespace SoulEngine::Scene
