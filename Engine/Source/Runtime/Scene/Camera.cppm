module;

#include <entt/entt.hpp>
#include <hlsl++.h>

export module Scene:Camera;

export import Core;
export import RHI;

export namespace SoulEngine {

/// @brief Renderer-neutral visibility and material targets for one camera/view.
struct GBufferTargets {
    RHIRef<RHIRenderTarget> AlbedoRT        = nullptr;
    RHIRef<RHIRenderTarget> NormalRT        = nullptr;
    RHIRef<RHIRenderTarget> EntityIdRT      = nullptr;
    RHIRef<RHIRenderTarget> WorldPositionRT = nullptr;
    RHIRef<RHIRenderTarget> DepthRT         = nullptr;
    RHIRef<RHIRenderTarget> LightingRT      = nullptr;
};

/// @brief Immutable render data and resources for one camera/view.
struct RenderViewSnapshot {
    hlslpp::float4x4 ViewProjection = hlslpp::float4x4::identity();
    hlslpp::float3   CameraPosition = hlslpp::float3(0.0f, 0.0f, 0.0f);
    Float32          ExposureEV100  = 15.0f;
    GBufferTargets   Visibility     = {};
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
    Float32                 ExposureEV100  = 15.0f;
    GBufferTargets          Visibility      = {};
    Uint32                  ViewportWidth  = 0;
    Uint32                  ViewportHeight = 0;

    /// @brief Resize the camera-owned output resources.
    auto ResizeViewport(StringView ResourceKey, Uint32 Width, Uint32 Height) -> void {
        if (Width == 0 || Height == 0) {
            Visibility      = {};
            ViewportWidth  = 0;
            ViewportHeight = 0;
            return;
        }

        if (ViewportWidth == Width && ViewportHeight == Height && Visibility.AlbedoRT && Visibility.NormalRT &&
            Visibility.EntityIdRT && Visibility.WorldPositionRT && Visibility.DepthRT && Visibility.LightingRT)
            return;

        ViewportWidth  = Width;
        ViewportHeight = Height;
        const RHIRenderTargetDesc ColorDesc{
            .Width  = Width,
            .Height = Height,
            .Format = RHIFormat::B8G8R8A8_UNORM,
            .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::FrameOutput | RHITextureUsage::ShaderStorage,
        };
        const RHIRenderTargetDesc NormalDesc{
            .Width  = Width,
            .Height = Height,
            .Format = RHIFormat::R16G16B16A16_SFLOAT,
            .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource | RHITextureUsage::ShaderStorage,
        };
        const RHIRenderTargetDesc EntityIdDesc{
            .Width  = Width,
            .Height = Height,
            .Format = RHIFormat::R32_UINT,
            .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource | RHITextureUsage::ShaderStorage,
        };
        const RHIRenderTargetDesc WorldPositionDesc{
            .Width  = Width,
            .Height = Height,
            .Format = RHIFormat::R16G16B16A16_SFLOAT,
            .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderStorage,
        };
        const RHIRenderTargetDesc DepthDesc{
            .Width  = Width,
            .Height = Height,
            .Format = RHIFormat::D32_SFLOAT,
            .Usage  = RHITextureUsage::DepthStencil,
        };
        const RHIRenderTargetDesc LightingDesc{
            .Width  = Width,
            .Height = Height,
            .Format = RHIFormat::B8G8R8A8_UNORM,
            .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::FrameOutput,
        };
        auto Color = RHIRenderDevice::Get().CreateRenderTarget(ColorDesc);
        if (!Color) {
            LogError("Failed to queue camera color render target creation: {}", Color.error().ToString());
            Visibility.AlbedoRT = nullptr;
        } else {
            Visibility.AlbedoRT = std::move(*Color);
        }

        auto Normal = RHIRenderDevice::Get().CreateRenderTarget(NormalDesc);
        if (!Normal) {
            LogError("Failed to queue camera normal render target creation: {}", Normal.error().ToString());
            Visibility.NormalRT = nullptr;
        } else {
            Visibility.NormalRT = std::move(*Normal);
        }

        auto EntityId = RHIRenderDevice::Get().CreateRenderTarget(EntityIdDesc);
        if (!EntityId) {
            LogError("Failed to queue camera entity ID render target creation: {}", EntityId.error().ToString());
            Visibility.EntityIdRT = nullptr;
        } else {
            Visibility.EntityIdRT = std::move(*EntityId);
        }

        auto WorldPosition = RHIRenderDevice::Get().CreateRenderTarget(WorldPositionDesc);
        if (!WorldPosition) {
            LogError("Failed to queue camera world-position render target creation: {}", WorldPosition.error().ToString());
            Visibility.WorldPositionRT = nullptr;
        } else {
            Visibility.WorldPositionRT = std::move(*WorldPosition);
        }

        auto Depth = RHIRenderDevice::Get().CreateRenderTarget(DepthDesc);
        if (!Depth) {
            LogError("Failed to queue camera depth render target creation: {}", Depth.error().ToString());
            Visibility.DepthRT = nullptr;
        } else {
            Visibility.DepthRT = std::move(*Depth);
        }

        auto Lighting = RHIRenderDevice::Get().CreateRenderTarget(LightingDesc);
        if (!Lighting) {
            LogError("Failed to queue camera lighting render target creation: {}", Lighting.error().ToString());
            Visibility.LightingRT = nullptr;
        } else {
            Visibility.LightingRT = std::move(*Lighting);
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
        if (!Visibility.AlbedoRT || !Visibility.NormalRT || !Visibility.EntityIdRT ||
            !Visibility.WorldPositionRT || !Visibility.DepthRT || !Visibility.LightingRT ||
            ViewportWidth == 0 || ViewportHeight == 0)
            return std::nullopt;

        auto VisibilityRef = Visibility;
        if (!VisibilityRef.AlbedoRT.TryGet() || !VisibilityRef.NormalRT.TryGet() ||
            !VisibilityRef.EntityIdRT.TryGet() || !VisibilityRef.WorldPositionRT.TryGet() ||
            !VisibilityRef.DepthRT.TryGet() || !VisibilityRef.LightingRT.TryGet())
            return std::nullopt;

        const float AspectRatio = static_cast<float>(ViewportWidth) / static_cast<float>(ViewportHeight);
        return RenderViewSnapshot{
            .ViewProjection = hlslpp::mul(GetViewMatrix(WorldTransform), GetProjectionMatrix(AspectRatio)),
            .CameraPosition = hlslpp::float3(WorldTransform[3].x, WorldTransform[3].y, WorldTransform[3].z),
            .ExposureEV100  = ExposureEV100,
            .Visibility     = std::move(VisibilityRef),
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
