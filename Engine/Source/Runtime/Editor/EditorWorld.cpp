module;

#include <entt/entt.hpp>
#include <hlsl++.h>
#include <imgui.h>

module Editor.EditorWorld;

import std;
import Core;
import Editor.EditorCamera;
import EditorTypes;
import RHI;
import WindowSystem;

namespace SoulEngine {

EditorWorld::EditorWorld() : m_SystemScheduler(m_Registry) {
    m_Registry.ctx().emplace<entt::dispatcher>();
}

auto EditorWorld::Initialize(IWindowSystem& Window) -> void {
    if (m_EditorRootEntity != entt::null)
        return;

    m_EditorRootEntity = m_Registry.create();
    m_Registry.emplace<TransformComponent>(m_EditorRootEntity);

    // EditorCameraSystem patches local transforms from input; it must run before TransformSystem so the
    // changes propagate within the same frame.
    const auto Setup =
        m_SystemScheduler.Register<EditorCameraSystem>("EditorCameraSystem", {}, {}, Window)
            .and_then([&]() -> std::expected<void, ErrorMessage> {
                return m_SystemScheduler.Register<TransformSystem>("TransformSystem", {}, {"EditorCameraSystem"});
            })
            .and_then([&]() -> std::expected<void, ErrorMessage> { return m_SystemScheduler.CompileDependency(); });
    if (!Setup) {
        LogError("Editor system setup failed:\n{}", Setup.error().ToString());
        return;
    }
    m_SystemScheduler.SetupObservers();

    const auto CameraEntity = m_Registry.create();
    m_Registry.emplace<ParentComponent>(CameraEntity, m_EditorRootEntity);
    m_Registry.emplace<TransformComponent>(CameraEntity,
                                           TransformComponent{
                                               .Translation = hlslpp::float3(0.0f, 2.0f, 0.0f),
                                               .Rotation    = hlslpp::float3(10.0f, 0.0f, 0.0f),
                                           });
    m_Registry.emplace<NameComponent>(CameraEntity, NameComponent{.Name = "EditorViewportCamera"});
    auto& Camera = m_Registry.emplace<EditorCameraComponent>(CameraEntity);
    if (const auto Readback = RHIRenderDevice::Get().CreateReadback("Editor/Viewport/Picking", sizeof(Uint32));
        Readback) {
        Camera.Readback = *Readback;
    } else {
        LogWarning("Editor viewport picking readback unavailable: {}", Readback.error().ToString());
    }

    BindWindowEvents(Window);
}

EditorWorld::~EditorWorld() {
    Shutdown();
}

auto EditorWorld::BindWindowEvents(IWindowSystem& Window) -> void {
    if (m_WindowEventDispatcher)
        return;

    m_WindowEventDispatcher = &Window.GetEventDispatcher();
    m_WindowEventDispatcher->sink<FramebufferResizeEvent>().connect<&EditorWorld::OnFramebufferResize>(*this);
    // The window was created before this subscription, so its initial framebuffer size did not produce a
    // callback. Trigger it explicitly to initialize the viewport camera through the same resize path.
    m_WindowEventDispatcher->trigger(FramebufferResizeEvent{
        .PreviousExtent = {},
        .CurrentExtent  = Window.GetFramebufferExtent(),
    });
}

auto EditorWorld::UnbindWindowEvents() -> void {
    if (!m_WindowEventDispatcher)
        return;

    m_WindowEventDispatcher->sink<FramebufferResizeEvent>().disconnect<&EditorWorld::OnFramebufferResize>(*this);
    m_WindowEventDispatcher = nullptr;
}

auto EditorWorld::Tick(Float32 DeltaTime) -> void {
    if (const auto Result = m_SystemScheduler.OnUpdate(DeltaTime); !Result)
        LogError("Editor system update failed:\n{}", Result.error().ToString());
    m_SystemScheduler.ClearObservers();
}

auto EditorWorld::BuildSnapshot() -> EditorSnapshot {
    const auto CameraSys = m_SystemScheduler.Get<EditorCameraSystem>();
    if (!CameraSys)
        return {};

    EditorSnapshot Snapshot{.Views = CameraSys->get().CollectViews()};
    for (const auto CameraEntity : m_Registry.view<EditorCameraComponent>()) {
        const auto& Camera      = m_Registry.get<EditorCameraComponent>(CameraEntity);
        Snapshot.SelectedEntity = Camera.SelectedEntity;
        Snapshot.ReadbackTarget = Camera.Readback;
        const auto&   IO        = ImGui::GetIO();
        const auto&   Vp        = Camera.Viewport;
        // Map the mouse position through the camera's Viewport rect. MousePos is in logical ImGui coords
        // while the viewport is in physical pixels — scale by DisplayFramebufferScale first.
        const Float32 MouseX    = IO.MousePos.x * IO.DisplayFramebufferScale.x;
        const Float32 MouseY    = IO.MousePos.y * IO.DisplayFramebufferScale.y;
        if (Vp.Width > 0 && Vp.Height > 0 && MouseX >= static_cast<Float32>(Vp.X) &&
            MouseY >= static_cast<Float32>(Vp.Y) && MouseX < static_cast<Float32>(Vp.X + Vp.Width) &&
            MouseY < static_cast<Float32>(Vp.Y + Vp.Height)) {
            Snapshot.HoverPixel = PixelCoordinate{
                .X = (std::min)(static_cast<Uint32>(MouseX - static_cast<Float32>(Vp.X)), Vp.Width - 1),
                .Y = (std::min)(static_cast<Uint32>(MouseY - static_cast<Float32>(Vp.Y)), Vp.Height - 1),
            };
            Snapshot.IsHovering = true;
        }
        break;
    }
    return Snapshot;
}

auto EditorWorld::Shutdown() -> void {
    UnbindWindowEvents();

    static_cast<void>(m_SystemScheduler.Remove<EditorCameraSystem>());
    static_cast<void>(m_SystemScheduler.Remove<TransformSystem>());

    // Clearing the registry releases component-held RHI references and allows EditorCameraSystem's cache
    // to be released with the system itself.
    m_Registry.clear();

    m_EditorRootEntity = entt::null;
}

auto EditorWorld::OnFramebufferResize(FramebufferResizeEvent& Event) -> void {
    auto& Dispatcher = m_Registry.ctx().get<entt::dispatcher>();
    for (const auto CameraEntity : m_Registry.view<EditorCameraComponent>()) {
        Dispatcher.enqueue<EditorCameraResizeEvent>(EditorCameraResizeEvent{
            .CameraEntity = CameraEntity,
            .Width        = static_cast<Uint32>(Event.CurrentExtent.Width),
            .Height       = static_cast<Uint32>(Event.CurrentExtent.Height),
        });
    }
}

} // namespace SoulEngine
