module;

#include <entt/entt.hpp>
#include <hlsl++.h>
#include <imgui.h>

module Editor.EditorCamera;

import std;
import Core;
import RHI;
import Scene;
import Renderer;
import WindowSystem;
import EditorTypes;

namespace SoulEngine {

auto EditorCameraSystem::OnUpdate(Float32 DeltaTime) -> void {
    m_Registry.ctx().get<entt::dispatcher>().update<EditorCameraResizeEvent>();
    UpdatePerspective(DeltaTime);
    UpdateSelection();
}

auto EditorCameraSystem::SetupObservers() -> void {
    m_Registry.ctx()
        .get<entt::dispatcher>()
        .sink<EditorCameraResizeEvent>()
        .connect<&EditorCameraSystem::OnCameraResize>(*this);
}

auto EditorCameraSystem::TeardownObservers() -> void {
    m_Registry.ctx()
        .get<entt::dispatcher>()
        .sink<EditorCameraResizeEvent>()
        .disconnect<&EditorCameraSystem::OnCameraResize>(*this);
}

auto EditorCameraSystem::CollectViews() const -> std::vector<EditorViewRecord> {
    std::vector<EditorViewRecord> Views;
    for (const auto Entity : m_Registry.view<EditorCameraComponent, TransformComponent>()) {
        const auto& Camera    = m_Registry.get<EditorCameraComponent>(Entity);
        const auto& Transform = m_Registry.get<TransformComponent>(Entity);
        if (Camera.Viewport.Width == 0 || Camera.Viewport.Height == 0)
            continue;

        const Float32 AspectRatio =
            static_cast<Float32>(Camera.Viewport.Width) / static_cast<Float32>(Camera.Viewport.Height);
        const Float32 FovRad     = Camera.FOV * (std::numbers::pi_v<Float32> / 180.0f);
        const auto    Projection = hlslpp::float4x4::perspective(
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

        Views.emplace_back(EditorViewRecord{
            .Camera =
                CameraViewRecord{
                    .ViewProjection = hlslpp::mul(View, Projection),
                    .CameraPosition = Position,
                    .ExposureEV100  = Camera.ExposureEV100,
                    .Viewport       = Camera.Viewport,
                },
        });
    }
    return Views;
}

auto EditorCameraSystem::BuildSnapshot() -> EditorSnapshot {
    EditorSnapshot Snapshot{.Views = CollectViews()};
    for (const auto CameraEntity : m_Registry.view<EditorCameraComponent>()) {
        auto& Camera                 = m_Registry.get<EditorCameraComponent>(CameraEntity);
        Snapshot.PrimaryViewport     = Camera.Viewport;
        Snapshot.SelectedEntity      = Camera.SelectedEntity;
        Snapshot.ReadbackEntity      = Camera.ReadbackEntity;
        Snapshot.ReadbackTarget      = Camera.Readback;
        Snapshot.ReadbackState       = Camera.ReadbackState;
        Snapshot.ReadbackReason      = Camera.ReadbackReason;
        Snapshot.IsReadbackMalformed = Camera.IsReadbackMalformed;
        Snapshot.IsReadbackPending   = Camera.Readback && Camera.IsReadbackScheduled;

        const auto& Vp = Camera.Viewport;
        if (ImGui::GetCurrentContext() && Vp.Width > 0 && Vp.Height > 0) {
            const auto&   IO     = ImGui::GetIO();
            // Map the mouse position through the camera's Viewport rect. MousePos is in logical ImGui coords
            // while the viewport is in physical pixels — scale by DisplayFramebufferScale first.
            const Float32 MouseX = IO.MousePos.x * IO.DisplayFramebufferScale.x;
            const Float32 MouseY = IO.MousePos.y * IO.DisplayFramebufferScale.y;
            if (MouseX >= static_cast<Float32>(Vp.X) && MouseY >= static_cast<Float32>(Vp.Y) &&
                MouseX < static_cast<Float32>(Vp.X + Vp.Width) && MouseY < static_cast<Float32>(Vp.Y + Vp.Height)) {
                Snapshot.HoverPixel = PixelCoordinate{
                    .X = (std::min)(static_cast<Uint32>(MouseX - static_cast<Float32>(Vp.X)), Vp.Width - 1),
                    .Y = (std::min)(static_cast<Uint32>(MouseY - static_cast<Float32>(Vp.Y)), Vp.Height - 1),
                };
                Snapshot.IsHovering = true;
            }
        }
        Camera.IsReadbackScheduled = Snapshot.IsHovering && static_cast<bool>(Camera.Readback);
        break;
    }
    return Snapshot;
}

auto EditorCameraSystem::UpdatePerspective(Float32 DeltaTime) -> void {
    if (!ImGui::GetCurrentContext())
        return;
    const auto& IO                = ImGui::GetIO();
    const bool  CameraInputActive = !IO.WantCaptureMouse && ImGui::IsMouseDown(ImGuiMouseButton_Right);
    m_Window->SetCursorMode(CameraInputActive ? CursorMode::Disabled : CursorMode::Normal);
    if (!CameraInputActive)
        return;

    const auto        CursorDelta  = IO.MouseDelta;
    const auto        ForwardAxis  = GetAxis(ImGuiKey_W, ImGuiKey_S);
    const auto        RightAxis    = GetAxis(ImGuiKey_D, ImGuiKey_A);
    const auto        VerticalAxis = GetAxis(ImGuiKey_E, ImGuiKey_Q);
    constexpr Float32 Sensitivity  = 0.0025f;
    constexpr Float32 MaxPitch     = 1.55334306f;

    for (const auto CameraEntity : m_Registry.view<EditorCameraComponent, TransformComponent>()) {
        const auto& CurrentTransform     = m_Registry.get<TransformComponent>(CameraEntity);
        const auto  EditorWorldTransform = CurrentTransform.GetLocalMatrix();
        const auto  WorldForward         = hlslpp::mul(hlslpp::float4(0.0f, 0.0f, -1.0f, 0.0f), EditorWorldTransform);
        const auto  Forward = hlslpp::normalize(hlslpp::float3(WorldForward.x, WorldForward.y, WorldForward.z));
        const auto  HorizontalForward = hlslpp::normalize(hlslpp::float3(Forward.x, 0.0f, Forward.z));
        const auto  Up                = hlslpp::float3(0.0f, 1.0f, 0.0f);
        const auto  Right             = hlslpp::normalize(hlslpp::cross(HorizontalForward, Up));
        const auto  MoveDirection     = HorizontalForward * ForwardAxis + Right * RightAxis + Up * VerticalAxis;

        m_Registry.patch<TransformComponent>(CameraEntity, [&](auto& Transform) -> void {
            if (MoveDirection.x != 0.0f || MoveDirection.y != 0.0f || MoveDirection.z != 0.0f)
                Transform.Translation += hlslpp::normalize(MoveDirection) * (2.0f * DeltaTime);
            Transform.Translation += Forward * (IO.MouseWheel * 0.75f);
            Transform.Rotation.y  += CursorDelta.x * Sensitivity * (180.0f / std::numbers::pi_v<Float32>);
            Transform.Rotation.x = std::clamp(static_cast<Float32>(Transform.Rotation.x) +
                                                  CursorDelta.y * Sensitivity * (180.0f / std::numbers::pi_v<Float32>),
                                              -MaxPitch * (180.0f / std::numbers::pi_v<Float32>),
                                              MaxPitch * (180.0f / std::numbers::pi_v<Float32>));
        });
    }
}

auto EditorCameraSystem::UpdateSelection() -> void {
    if (!ImGui::GetCurrentContext())
        return;
    const auto& IO = ImGui::GetIO();
    for (const auto CameraEntity : m_Registry.view<EditorCameraComponent>()) {
        auto& Camera = m_Registry.get<EditorCameraComponent>(CameraEntity);
        if (!Camera.Readback) {
            Camera.ReadbackEntity      = std::nullopt;
            Camera.ReadbackState       = EditorReadbackState::Unavailable;
            Camera.ReadbackReason      = "readback_unavailable";
            Camera.IsReadbackScheduled = false;
            Camera.IsReadbackMalformed = false;
        } else {
            Camera.IsReadbackMalformed = false;
            const auto Bytes           = Camera.Readback->TryRead();
            if (Bytes.empty()) {
                if (Camera.ReadbackState != EditorReadbackState::LatestKnown) {
                    Camera.ReadbackState = EditorReadbackState::Unavailable;
                    Camera.ReadbackReason =
                        Camera.IsReadbackScheduled ? "scheduled_readback_incomplete" : "no_completed_readback";
                } else if (Camera.IsReadbackScheduled) {
                    Camera.ReadbackReason = "scheduled_readback_incomplete";
                } else {
                    Camera.ReadbackReason.clear();
                }
            } else if (Bytes.size() != sizeof(Uint32)) {
                Camera.IsReadbackMalformed = true;
                Camera.ReadbackState       = Camera.ReadbackState == EditorReadbackState::LatestKnown
                                                 ? EditorReadbackState::LatestKnown
                                                 : EditorReadbackState::Malformed;
                Camera.ReadbackReason      = "malformed_payload";
            } else {
                Uint32 EntityId;
                std::memcpy(&EntityId, Bytes.data(), sizeof(EntityId));
                Camera.ReadbackEntity      = EntityId == GBuffer::BackgroundEntityId
                                                 ? std::nullopt
                                                 : std::optional<entt::entity>{static_cast<entt::entity>(EntityId)};
                Camera.IsReadbackMalformed = false;
                Camera.ReadbackState       = EditorReadbackState::LatestKnown;
                Camera.ReadbackReason      = Camera.IsReadbackScheduled ? "scheduled_readback_incomplete" : "";
            }
        }
        // The click guard mirrors the hover mapping: mouse in logical coords, viewport in physical pixels
        // — scale by DisplayFramebufferScale before the rect test (DPI awareness).
        const auto& Vp = Camera.Viewport;
        if (Vp.Width == 0 || Vp.Height == 0)
            continue;
        const Float32 MouseX = IO.MousePos.x * IO.DisplayFramebufferScale.x;
        const Float32 MouseY = IO.MousePos.y * IO.DisplayFramebufferScale.y;
        if (MouseX < static_cast<Float32>(Vp.X) || MouseY < static_cast<Float32>(Vp.Y) ||
            MouseX >= static_cast<Float32>(Vp.X + Vp.Width) || MouseY >= static_cast<Float32>(Vp.Y + Vp.Height))
            continue;
        if (!IO.WantCaptureMouse && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            Camera.SelectedEntity = Camera.ReadbackEntity;
        }
    }
}

auto EditorCameraSystem::OnCameraResize(EditorCameraResizeEvent& Event) -> void {
    if (!m_Registry.valid(Event.CameraEntity) || !m_Registry.all_of<EditorCameraComponent>(Event.CameraEntity))
        return;

    auto& Component = m_Registry.get<EditorCameraComponent>(Event.CameraEntity);
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

auto EditorCameraSystem::GetAxis(ImGuiKey Positive, ImGuiKey Negative) const -> Float32 {
    return (ImGui::IsKeyDown(Positive) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(Negative) ? 1.0f : 0.0f);
}

} // namespace SoulEngine
