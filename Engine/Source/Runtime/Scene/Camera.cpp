module;

#include <entt/entt.hpp>
#include <hlsl++.h>

module Scene:Camera.impl;

import :Camera;

namespace SoulEngine {

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

auto CameraSystem::OnUpdate(Float32 DeltaTime) -> void {
    m_Registry.ctx().get<entt::dispatcher>().update<CameraResizeEvent>();
}

auto CameraSystem::SetupObservers() -> void {
    m_Registry.ctx().get<entt::dispatcher>().sink<CameraResizeEvent>().connect<&CameraSystem::OnCameraResize>(*this);
}

auto CameraSystem::TeardownObservers() -> void {
    m_Registry.ctx().get<entt::dispatcher>().sink<CameraResizeEvent>().disconnect<&CameraSystem::OnCameraResize>(*this);
}

auto CameraSystem::CollectViews() const -> std::vector<CameraViewRecord> {
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

        const auto Projection = hlslpp::float4x4::perspective(
            hlslpp::projection(hlslpp::frustum::field_of_view_y(FovRad, AspectRatio, Camera.NearPlane, Camera.FarPlane),
                               hlslpp::zclip::zero,
                               hlslpp::zdirection::forward,
                               hlslpp::zplane::finite));

        const auto Position =
            hlslpp::float3(Transform.WorldTransform[3].x, Transform.WorldTransform[3].y, Transform.WorldTransform[3].z);
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

auto CameraSystem::OnCameraResize(CameraResizeEvent& Event) -> void {
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

} // namespace SoulEngine
