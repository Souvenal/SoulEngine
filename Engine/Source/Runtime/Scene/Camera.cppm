module;

#include <entt/entt.hpp>
#include <hlsl++.h>

export module Scene:Camera;

export import Core;
export import RHI;

export namespace SoulEngine {

/// @brief G-buffer resources for one camera/view.
struct GBuffer {
    static constexpr Uint32                                      ColorAttachmentCount = 4;
    static constexpr RHIFormat                                   DepthFormat          = RHIFormat::D32_SFLOAT;
    static constexpr std::array<RHIFormat, ColorAttachmentCount> ColorFormats         = {
        RHIFormat::B8G8R8A8_UNORM,
        RHIFormat::R16G16B16A16_SFLOAT,
        RHIFormat::R32_UINT,
        RHIFormat::R32_UINT,
    };
    static constexpr Uint32 BackgroundEntityId = std::numeric_limits<Uint32>::max();

    RHIRef<RHIRenderTarget> AlbedoRT     = nullptr;
    RHIRef<RHIRenderTarget> NormalRT     = nullptr;
    RHIRef<RHIRenderTarget> MaterialIdRT = nullptr;
    RHIRef<RHIRenderTarget> EntityIdRT   = nullptr;
    RHIRef<RHIRenderTarget> DepthRT      = nullptr;

    /// @brief Return whether every G-buffer attachment is ready for rendering.
    [[nodiscard]] auto IsValid() const -> bool {
        return AlbedoRT && NormalRT && MaterialIdRT && EntityIdRT && DepthRT;
    }
};

/// @brief Render targets shared by all passes for one camera/view.
struct CameraRenderTargets {
    GBuffer                 GBuffer      = {};
    RHIRef<RHIRenderTarget> SceneColorRT = nullptr;

    /// @brief Return whether all camera render targets are ready for rendering.
    [[nodiscard]] auto IsValid() const -> bool {
        return GBuffer.IsValid() && SceneColorRT;
    }
};

/// @brief Request to resize one camera's render targets.
struct CameraResizeEvent {
    entt::entity CameraEntity = entt::null;
    Uint32       Width        = 0;
    Uint32       Height       = 0;
};

/// @brief Immutable render data and resources for one camera/view.
struct CameraViewRecord {
    hlslpp::float4x4    ViewProjection = hlslpp::float4x4::identity();
    hlslpp::float3      CameraPosition = hlslpp::float3(0.0f, 0.0f, 0.0f);
    Float32             ExposureEV100  = 15.0f;
    CameraRenderTargets Targets        = {};
};

/// @brief Resource loader for named CameraRenderTargets.
///
/// Creates render targets based on a camera debug name and viewport dimensions.
struct CameraRenderTargetsLoader {
    using result_type = std::shared_ptr<CameraRenderTargets>;

    auto operator()(StringView ResourceName, Uint32 Width, Uint32 Height) const -> result_type {
        if (Width == 0 || Height == 0) {
            return nullptr;
        }

        auto       Targets        = std::make_shared<CameraRenderTargets>();
        const auto AlbedoName     = Format("{}/GBuffer/Albedo", ResourceName);
        const auto NormalName     = Format("{}/GBuffer/Normal", ResourceName);
        const auto EntityIdName   = Format("{}/GBuffer/EntityId", ResourceName);
        const auto MaterialIdName = Format("{}/GBuffer/MaterialId", ResourceName);
        const auto DepthName      = Format("{}/GBuffer/Depth", ResourceName);
        const auto SceneColorName = Format("{}/SceneColor", ResourceName);

        const RHIRenderTargetDesc ColorDesc{
            .Width  = Width,
            .Height = Height,
            .Format = RHIFormat::B8G8R8A8_UNORM,
            .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::FrameOutput | RHITextureUsage::ShaderResource,
        };
        const RHIRenderTargetDesc NormalDesc{
            .Width  = Width,
            .Height = Height,
            .Format = RHIFormat::R16G16B16A16_SFLOAT,
            .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource,
        };
        const RHIRenderTargetDesc EntityIdDesc{
            .Width  = Width,
            .Height = Height,
            .Format = RHIFormat::R32_UINT,
            .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource,
        };
        const RHIRenderTargetDesc MaterialIdDesc{
            .Width  = Width,
            .Height = Height,
            .Format = RHIFormat::R32_UINT,
            .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource,
        };
        const RHIRenderTargetDesc DepthDesc{
            .Width  = Width,
            .Height = Height,
            .Format = RHIFormat::D32_SFLOAT,
            .Usage  = RHITextureUsage::DepthStencil | RHITextureUsage::ShaderResource,
        };
        const RHIRenderTargetDesc SceneColorDesc{
            .Width  = Width,
            .Height = Height,
            .Format = RHIFormat::B8G8R8A8_UNORM,
            .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::FrameOutput,
        };

        auto Albedo = RHIRenderDevice::Get().CreateRenderTarget(AlbedoName, ColorDesc);
        if (!Albedo) {
            LogError("Failed to queue camera albedo render target creation: {}", Albedo.error().ToString());
            Targets->GBuffer.AlbedoRT = nullptr;
        } else {
            Targets->GBuffer.AlbedoRT = std::move(*Albedo);
        }

        auto Normal = RHIRenderDevice::Get().CreateRenderTarget(NormalName, NormalDesc);
        if (!Normal) {
            LogError("Failed to queue camera normal render target creation: {}", Normal.error().ToString());
            Targets->GBuffer.NormalRT = nullptr;
        } else {
            Targets->GBuffer.NormalRT = std::move(*Normal);
        }

        auto EntityId = RHIRenderDevice::Get().CreateRenderTarget(EntityIdName, EntityIdDesc);
        if (!EntityId) {
            LogError("Failed to queue camera entity ID render target creation: {}", EntityId.error().ToString());
            Targets->GBuffer.EntityIdRT = nullptr;
        } else {
            Targets->GBuffer.EntityIdRT = std::move(*EntityId);
        }

        auto MaterialId = RHIRenderDevice::Get().CreateRenderTarget(MaterialIdName, MaterialIdDesc);
        if (!MaterialId) {
            LogError("Failed to queue camera material ID render target creation: {}", MaterialId.error().ToString());
            Targets->GBuffer.MaterialIdRT = nullptr;
        } else {
            Targets->GBuffer.MaterialIdRT = std::move(*MaterialId);
        }

        auto Depth = RHIRenderDevice::Get().CreateRenderTarget(DepthName, DepthDesc);
        if (!Depth) {
            LogError("Failed to queue camera depth render target creation: {}", Depth.error().ToString());
            Targets->GBuffer.DepthRT = nullptr;
        } else {
            Targets->GBuffer.DepthRT = std::move(*Depth);
        }

        auto SceneColor = RHIRenderDevice::Get().CreateRenderTarget(SceneColorName, SceneColorDesc);
        if (!SceneColor) {
            LogError("Failed to queue camera scene color render target creation: {}", SceneColor.error().ToString());
            Targets->SceneColorRT = nullptr;
        } else {
            Targets->SceneColorRT = std::move(*SceneColor);
        }

        return Targets;
    }
};

/// @brief Cache type for CameraRenderTargets resources.
using CameraRenderTargetsCache = entt::resource_cache<CameraRenderTargets, CameraRenderTargetsLoader>;

/// @brief Resource handle for CameraRenderTargets.
using CameraRenderTargetsHandle = entt::resource<CameraRenderTargets>;

/// @brief CameraComponent persists only the camera lens parameters.
///
/// Camera entities that allocate render targets must also have a non-empty
/// NameComponent. Camera output resources are managed as entt::resource handles.
struct CameraComponent {
    float                     FOV            = 60.0f;
    float                     NearPlane      = 0.1f;
    float                     FarPlane       = 100.0f;
    Float32                   ExposureEV100  = 15.0f;
    Uint32                    ViewportWidth  = 0;
    Uint32                    ViewportHeight = 0;
    CameraRenderTargetsHandle Targets        = {};

    /// @brief Return whether this camera can produce a render view record.
    [[nodiscard]] auto IsValid() const -> bool {
        return Targets && Targets->IsValid() && ViewportWidth != 0 && ViewportHeight != 0;
    }
};

/// @brief CameraSystem manages camera viewport resizing and render target creation.
///
/// Iterates over all CameraComponents and handles viewport changes.
class CameraSystem : public ISystem {
  public:
    explicit CameraSystem(entt::registry& Registry) : ISystem(Registry) {}

    /// @brief Update camera state and process queued resize events.
    /// @param DeltaTime Time elapsed since last frame in seconds.
    auto OnUpdate(Float32 DeltaTime) -> void override {
        m_Registry.ctx().get<entt::dispatcher>().update<CameraResizeEvent>();
    }

    /// @brief Subscribe to camera resize events.
    auto SetupObservers() -> void override {
        m_Registry.ctx().get<entt::dispatcher>().sink<CameraResizeEvent>().connect<&CameraSystem::OnCameraResize>(
            *this);
    }

    /// @brief Unsubscribe from camera resize events.
    auto TeardownObservers() -> void override {
        m_Registry.ctx().get<entt::dispatcher>().sink<CameraResizeEvent>().disconnect<&CameraSystem::OnCameraResize>(
            *this);
    }

    /// @brief Collect all valid camera view snapshots from the registry.
    /// @return Vector of CameraViewRecord for all valid cameras.
    [[nodiscard]] auto CollectViews() const -> std::vector<CameraViewRecord> {
        std::vector<CameraViewRecord> Views;

        const auto CameraView = m_Registry.view<CameraComponent, TransformComponent>();
        for (const auto Entity : CameraView) {
            const auto& Camera    = CameraView.get<CameraComponent>(Entity);
            const auto& Transform = CameraView.get<TransformComponent>(Entity);
            if (!Camera.IsValid())
                continue;

            const float AspectRatio =
                static_cast<float>(Camera.ViewportWidth) / static_cast<float>(Camera.ViewportHeight);
            const float FovRad = Camera.FOV * (std::numbers::pi_v<float> / 180.0f);

            const auto Projection = hlslpp::float4x4::perspective(hlslpp::projection(
                hlslpp::frustum::field_of_view_y(FovRad, AspectRatio, Camera.NearPlane, Camera.FarPlane),
                hlslpp::zclip::zero,
                hlslpp::zdirection::forward,
                hlslpp::zplane::finite));

            const auto Position = hlslpp::float3(
                Transform.WorldTransform[3].x, Transform.WorldTransform[3].y, Transform.WorldTransform[3].z);
            const auto LocalForward = hlslpp::float4(0.0f, 0.0f, -1.0f, 0.0f);
            const auto WorldForward = hlslpp::mul(LocalForward, Transform.WorldTransform);
            const auto Forward      = hlslpp::normalize(hlslpp::float3(WorldForward.x, WorldForward.y, WorldForward.z));

            const auto View = hlslpp::float4x4::look_at(Position, Position + Forward, hlslpp::float3(0.0f, 1.0f, 0.0f));

            Views.emplace_back(CameraViewRecord{
                .ViewProjection = hlslpp::mul(View, Projection),
                .CameraPosition = Position,
                .ExposureEV100  = Camera.ExposureEV100,
                .Targets        = *Camera.Targets,
            });
        }

        return Views;
    }

  private:
    auto OnCameraResize(CameraResizeEvent& Event) -> void {
        if (!m_Registry.valid(Event.CameraEntity) || !m_Registry.all_of<CameraComponent>(Event.CameraEntity))
            return;
        if (!m_Registry.all_of<NameComponent>(Event.CameraEntity)) {
            LogError("Camera entity {} has no NameComponent", entt::to_integral(Event.CameraEntity));
            return;
        }

        auto& Component = m_Registry.get<CameraComponent>(Event.CameraEntity);
        if (Event.Width == 0 || Event.Height == 0) {
            Component.Targets        = {};
            Component.ViewportWidth  = 0;
            Component.ViewportHeight = 0;
            return;
        }

        if (Component.ViewportWidth == Event.Width && Component.ViewportHeight == Event.Height && Component.Targets &&
            Component.Targets->GBuffer.AlbedoRT)
            return;

        Component.ViewportWidth  = Event.Width;
        Component.ViewportHeight = Event.Height;

        const auto& Name = m_Registry.get<NameComponent>(Event.CameraEntity).Name;

        const auto ResourceKey = Format("Camera/{}/{}", Name, entt::to_integral(Event.CameraEntity));
        const auto ResourceId  = entt::hashed_string{ResourceKey.data(), ResourceKey.size()};
        auto [It, Loaded]      = m_Cache.force_load(ResourceId, ResourceKey, Event.Width, Event.Height);
        if (It->second) {
            Component.Targets = It->second;
        } else {
            LogError("Failed to load CameraRenderTargets for '{}'", ResourceKey);
            Component.Targets = {};
        }
    }

    CameraRenderTargetsCache m_Cache = {};
};

} // namespace SoulEngine

namespace SoulEngine {

namespace {

struct CameraComponentMetaRegistration {
    CameraComponentMetaRegistration() {
        entt::meta_factory<CameraComponent>{}
            .type("camera")
            .data<&CameraComponent::FOV>("fov_degrees")
            .data<&CameraComponent::NearPlane>("near_plane")
            .data<&CameraComponent::FarPlane>("far_plane")
            .data<&CameraComponent::ExposureEV100>("exposure_ev100");
    }
};

CameraComponentMetaRegistration g_CameraComponentMetaRegistration = {};

} // namespace

} // namespace SoulEngine
