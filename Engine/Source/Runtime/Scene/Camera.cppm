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
};

/// @brief Render targets shared by all passes for one camera/view.
struct ViewRenderTargets {
    GBuffer                 GBuffer      = {};
    RHIRef<RHIRenderTarget> SceneColorRT = nullptr;
};

/// @brief Immutable render data and resources for one camera/view.
struct RenderViewSnapshot {
    hlslpp::float4x4  ViewProjection = hlslpp::float4x4::identity();
    hlslpp::float3    CameraPosition = hlslpp::float3(0.0f, 0.0f, 0.0f);
    Float32           ExposureEV100  = 15.0f;
    ViewRenderTargets Targets        = {};
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
    float             FOV            = 60.0f;
    float             NearPlane      = 0.1f;
    float             FarPlane       = 100.0f;
    Float32           ExposureEV100  = 15.0f;
    ViewRenderTargets Targets        = {};
    Uint32            ViewportWidth  = 0;
    Uint32            ViewportHeight = 0;

    /// @brief Resize the camera-owned output resources.
    auto ResizeViewport(StringView ResourceKey, Uint32 Width, Uint32 Height) -> void {
        if (Width == 0 || Height == 0) {
            Targets        = {};
            ViewportWidth  = 0;
            ViewportHeight = 0;
            return;
        }

        if (ViewportWidth == Width && ViewportHeight == Height && Targets.GBuffer.AlbedoRT &&
            Targets.GBuffer.NormalRT && Targets.GBuffer.EntityIdRT && Targets.GBuffer.MaterialIdRT &&
            Targets.GBuffer.DepthRT && Targets.SceneColorRT)
            return;

        ViewportWidth  = Width;
        ViewportHeight = Height;
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
        auto Color = RHIRenderDevice::Get().CreateRenderTarget(Format("{}/GBuffer/Albedo", ResourceKey), ColorDesc);
        if (!Color) {
            LogError("Failed to queue camera color render target creation: {}", Color.error().ToString());
            Targets.GBuffer.AlbedoRT = nullptr;
        } else {
            Targets.GBuffer.AlbedoRT = std::move(*Color);
        }

        auto Normal = RHIRenderDevice::Get().CreateRenderTarget(Format("{}/GBuffer/Normal", ResourceKey), NormalDesc);
        if (!Normal) {
            LogError("Failed to queue camera normal render target creation: {}", Normal.error().ToString());
            Targets.GBuffer.NormalRT = nullptr;
        } else {
            Targets.GBuffer.NormalRT = std::move(*Normal);
        }

        auto EntityId = RHIRenderDevice::Get().CreateRenderTarget(Format("{}/GBuffer/EntityId", ResourceKey), EntityIdDesc);
        if (!EntityId) {
            LogError("Failed to queue camera entity ID render target creation: {}", EntityId.error().ToString());
            Targets.GBuffer.EntityIdRT = nullptr;
        } else {
            Targets.GBuffer.EntityIdRT = std::move(*EntityId);
        }

        auto MaterialId =
            RHIRenderDevice::Get().CreateRenderTarget(Format("{}/GBuffer/MaterialId", ResourceKey), MaterialIdDesc);
        if (!MaterialId) {
            LogError("Failed to queue camera material ID render target creation: {}", MaterialId.error().ToString());
            Targets.GBuffer.MaterialIdRT = nullptr;
        } else {
            Targets.GBuffer.MaterialIdRT = std::move(*MaterialId);
        }

        auto Depth = RHIRenderDevice::Get().CreateRenderTarget(Format("{}/GBuffer/Depth", ResourceKey), DepthDesc);
        if (!Depth) {
            LogError("Failed to queue camera depth render target creation: {}", Depth.error().ToString());
            Targets.GBuffer.DepthRT = nullptr;
        } else {
            Targets.GBuffer.DepthRT = std::move(*Depth);
        }

        auto Lighting = RHIRenderDevice::Get().CreateRenderTarget(Format("{}/SceneColor", ResourceKey), SceneColorDesc);
        if (!Lighting) {
            LogError("Failed to queue camera lighting render target creation: {}", Lighting.error().ToString());
            Targets.SceneColorRT = nullptr;
        } else {
            Targets.SceneColorRT = std::move(*Lighting);
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

    [[nodiscard]] auto GetForward(const hlslpp::float4x4& WorldTransform) const -> hlslpp::float3 {
        const auto LocalForward = hlslpp::float4(0.0f, 0.0f, -1.0f, 0.0f);
        const auto WorldForward = hlslpp::mul(LocalForward, WorldTransform);
        return hlslpp::normalize(hlslpp::float3(WorldForward.x, WorldForward.y, WorldForward.z));
    }

    [[nodiscard]] auto GetViewMatrix(const hlslpp::float4x4& WorldTransform) const -> hlslpp::float4x4 {
        const auto Position = hlslpp::float3(WorldTransform[3].x, WorldTransform[3].y, WorldTransform[3].z);
        return hlslpp::float4x4::look_at(
            Position, Position + GetForward(WorldTransform), hlslpp::float3(0.0f, 1.0f, 0.0f));
    }

    [[nodiscard]] auto BuildRenderView(const hlslpp::float4x4& WorldTransform) const
        -> std::optional<RenderViewSnapshot> {
        if (!Targets.GBuffer.AlbedoRT || !Targets.GBuffer.NormalRT || !Targets.GBuffer.EntityIdRT ||
            !Targets.GBuffer.MaterialIdRT || !Targets.GBuffer.DepthRT || !Targets.SceneColorRT || ViewportWidth == 0 ||
            ViewportHeight == 0)
            return std::nullopt;

        const float AspectRatio = static_cast<float>(ViewportWidth) / static_cast<float>(ViewportHeight);
        return RenderViewSnapshot{
            .ViewProjection = hlslpp::mul(GetViewMatrix(WorldTransform), GetProjectionMatrix(AspectRatio)),
            .CameraPosition = hlslpp::float3(WorldTransform[3].x, WorldTransform[3].y, WorldTransform[3].z),
            .ExposureEV100  = ExposureEV100,
            .Targets        = Targets,
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

    [[nodiscard]] auto GetExposureEV100() const -> Float32 {
        return Settings.ExposureEV100;
    }

    auto SetExposureEV100(Float32 Value) -> void {
        Settings.ExposureEV100 = Value;
    }
};

} // namespace SoulEngine

namespace SoulEngine {

namespace {

struct CameraComponentMetaRegistration {
    CameraComponentMetaRegistration() {
        entt::meta_factory<CameraComponent>{}
            .type("camera")
            .data<&CameraComponent::SetFOV, &CameraComponent::GetFOV>("fov_degrees")
            .data<&CameraComponent::SetNearPlane, &CameraComponent::GetNearPlane>("near_plane")
            .data<&CameraComponent::SetFarPlane, &CameraComponent::GetFarPlane>("far_plane")
            .data<&CameraComponent::SetExposureEV100, &CameraComponent::GetExposureEV100>("exposure_ev100");
    }
};

CameraComponentMetaRegistration g_CameraComponentMetaRegistration = {};

} // namespace

} // namespace SoulEngine
