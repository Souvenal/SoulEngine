module;

#include <entt/entt.hpp>
#include <hlsl++.h>

export module Scene:Camera;

import std;
export import Core;

export namespace SoulEngine {

/// @brief Physical-pixel rectangle on the window where a camera's final image
/// lands (ADR 05). The present blit targets it; picking maps through it.
struct CameraViewport {
    Uint32 X      = 0;
    Uint32 Y      = 0;
    Uint32 Width  = 0;
    Uint32 Height = 0;
};

/// @brief Request to change one camera's viewport.
struct CameraResizeEvent {
    entt::entity CameraEntity = entt::null;
    Uint32       X            = 0;
    Uint32       Y            = 0;
    Uint32       Width        = 0;
    Uint32       Height       = 0;
};

/// @brief Immutable render data for one camera/view.
///
/// Carries no RHI resources (ADR 05): renderers create the view's render
/// targets as pooled RenderGraph transients derived from the Viewport.
struct CameraViewRecord {
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
/// Viewport is Runtime State driven by CameraResizeEvent, not authoring data;
/// it is intentionally absent from the EnTT meta registration below. View
/// render targets are not component state (ADR 05).
struct CameraComponent {
    float          FOV            = 60.0f;
    float          NearPlane      = 0.1f;
    float          FarPlane       = 100.0f;
    Float32        ExposureEV100  = 15.0f;
    CameraViewport Viewport       = {};

    /// @brief Return whether this camera can produce a render view record.
    [[nodiscard]] auto IsValid() const -> bool {
        return Viewport.Width != 0 && Viewport.Height != 0;
    }
};

/// @brief CameraSystem manages camera viewport changes.
///
/// Iterates over all CameraComponents and handles viewport change events.
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
                static_cast<float>(Camera.Viewport.Width) / static_cast<float>(Camera.Viewport.Height);
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
                .Viewport       = Camera.Viewport,
            });
        }

        return Views;
    }

  private:
    auto OnCameraResize(CameraResizeEvent& Event) -> void {
        if (!m_Registry.valid(Event.CameraEntity) || !m_Registry.all_of<CameraComponent>(Event.CameraEntity))
            return;

        auto& Component = m_Registry.get<CameraComponent>(Event.CameraEntity);
        if (Event.Width == 0 || Event.Height == 0) {
            Component.Viewport = {};
            return;
        }

        Component.Viewport = CameraViewport{
            .X      = Event.X,
            .Y      = Event.Y,
            .Width  = Event.Width,
            .Height = Event.Height,
        };
    }
};

} // namespace SoulEngine

namespace SoulEngine {

// Note: this namespace must stay named. In a named module, clang 23 silently
// drops the dynamic initializer of an unreferenced anonymous-namespace variable,
// which would skip this entt meta registration at program startup.
namespace MetaRegistration {

using namespace entt::literals;

struct CameraComponentMetaRegistration {
    CameraComponentMetaRegistration() {
        entt::meta_factory<CameraComponent>{"camera"_hs}
            .data<&CameraComponent::FOV>("fov_degrees"_hs)
            .data<&CameraComponent::NearPlane>("near_plane"_hs)
            .data<&CameraComponent::FarPlane>("far_plane"_hs)
            .data<&CameraComponent::ExposureEV100>("exposure_ev100"_hs)
            .func<&EmplaceComponent<CameraComponent>>("emplace"_hs);
    }
};

CameraComponentMetaRegistration g_CameraComponentMetaRegistration = {};

} // namespace MetaRegistration

} // namespace SoulEngine
