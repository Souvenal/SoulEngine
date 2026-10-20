module;

#include <entt/entt.hpp>
#include <hlsl++.h>

export module Scene:Camera;

import std;
import Core;

namespace SoulEngine {

/// @brief Physical-pixel rectangle on the window where a camera's final image
/// lands. The present blit targets it; picking maps through it.
export struct CameraViewport {
    Uint32 X      = 0;
    Uint32 Y      = 0;
    Uint32 Width  = 0;
    Uint32 Height = 0;
};

/// @brief Request to change one camera's viewport.
export struct CameraResizeEvent {
    entt::entity CameraEntity = entt::null;
    Uint32       X            = 0;
    Uint32       Y            = 0;
    Uint32       Width        = 0;
    Uint32       Height       = 0;
};

/// @brief Immutable render data for one camera/view.
///
/// Carries no RHI resources: renderers create the view's render targets as pooled RenderGraph transients
/// derived from the Viewport.
export struct CameraViewRecord {
    hlslpp::float4x4 ViewProjection = hlslpp::float4x4::identity();
    hlslpp::float3   CameraPosition = hlslpp::float3(0.0f, 0.0f, 0.0f);
    Float32          ExposureEV100  = 15.0f;
    CameraViewport   Viewport       = {};

    /// @brief Return the viewport width in physical pixels.
    [[nodiscard]] auto GetWidth() const -> Uint32 {
        return Viewport.Width;
    }

    /// @brief Return the viewport height in physical pixels.
    [[nodiscard]] auto GetHeight() const -> Uint32 {
        return Viewport.Height;
    }
};

/// @brief CameraComponent persists only the camera lens parameters and the
/// viewport rectangle.
///
/// Viewport is Runtime State driven by CameraResizeEvent, not authoring data; it is intentionally absent from
/// the EnTT meta registration below. View render targets are not component state.
export struct CameraComponent {
    float          FOV           = 60.0f;
    float          NearPlane     = 0.1f;
    float          FarPlane      = 100.0f;
    Float32        ExposureEV100 = 15.0f;
    CameraViewport Viewport      = {};

    /// @brief Return whether this camera can produce a render view record.
    [[nodiscard]] auto IsValid() const -> bool {
        return Viewport.Width != 0 && Viewport.Height != 0;
    }
};

/// @brief CameraSystem manages camera viewport changes.
///
/// Iterates over all CameraComponents and handles viewport change events.
export class CameraSystem : public ISystem {
  public:
    explicit CameraSystem(entt::registry& Registry) : ISystem(Registry) {}

    /// @brief Update camera state and process queued resize events.
    /// @param DeltaTime Time elapsed since last frame in seconds.
    auto OnUpdate(Float32 DeltaTime) -> void override;

    /// @brief Subscribe to camera resize events.
    auto SetupObservers() -> void override;

    /// @brief Unsubscribe from camera resize events.
    auto TeardownObservers() -> void override;

    /// @brief Collect all valid camera view snapshots from the registry.
    /// @return Vector of CameraViewRecord for all valid cameras.
    [[nodiscard]] auto CollectViews() const -> std::vector<CameraViewRecord>;

  private:
    auto OnCameraResize(CameraResizeEvent& Event) -> void;
};

} // namespace SoulEngine
