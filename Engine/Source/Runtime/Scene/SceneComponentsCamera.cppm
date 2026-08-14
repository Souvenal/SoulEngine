module;

#include <entt/entt.hpp>
#include <hlsl++.h>

export module Scene:Components.Camera;

import :Components.Core;

export namespace SoulEngine {

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
            .ExposureEV100  = ExposureEV100,
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

[[nodiscard]] auto ValidateCameraComponent(const SceneComponentValidationContext&,
                                           entt::registry& Registry,
                                           SceneEntity     Entity,
                                           String&         Error) -> bool {
    const auto* Camera = Registry.try_get<CameraComponent>(Entity);
    if (!Camera) {
        Error = "Camera component metadata does not contain CameraComponent";
        return false;
    }
    if (Camera->Settings.FOV <= 0.0f || Camera->Settings.FOV >= 179.0f) {
        Error = "fov_degrees must be in the range (0, 179)";
        return false;
    }
    if (Camera->Settings.NearPlane <= 0.0f) {
        Error = "near_plane must be greater than zero";
        return false;
    }
    if (Camera->Settings.FarPlane <= Camera->Settings.NearPlane) {
        Error = "far_plane must be greater than near_plane";
        return false;
    }
    if (!std::isfinite(Camera->Settings.ExposureEV100)) {
        Error = "exposure_ev100 must be finite";
        return false;
    }
    return true;
}

struct CameraComponentMetaRegistration {
    CameraComponentMetaRegistration() {
        entt::meta_factory<CameraComponent>{}
            .type("camera")
            .custom<SceneComponentSchema>(SceneComponentSchema{
                .Create   = &CreateSceneComponent<CameraComponent>,
                .Remove   = &RemoveSceneComponent<CameraComponent>,
                .Has      = &HasSceneComponent<CameraComponent>,
                .Validate = &ValidateCameraComponent,
            })
            .data<&CameraComponent::SetFOV, &CameraComponent::GetFOV>("fov_degrees")
            .data<&CameraComponent::SetNearPlane, &CameraComponent::GetNearPlane>("near_plane")
            .data<&CameraComponent::SetFarPlane, &CameraComponent::GetFarPlane>("far_plane")
            .data<&CameraComponent::SetExposureEV100, &CameraComponent::GetExposureEV100>("exposure_ev100");
    }
};

CameraComponentMetaRegistration g_CameraComponentMetaRegistration = {};

} // namespace

} // namespace SoulEngine
