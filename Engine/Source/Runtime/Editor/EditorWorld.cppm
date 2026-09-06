module;

#include <entt/entt.hpp>
#include <hlsl++.h>
#include <imgui.h>

export module Editor:EditorWorld;

import std;
import Core;
import :EditorCamera;
import EditorTypes;
import RHI;
import Scene;
import WindowSystem;

export namespace SoulEngine {

/// @brief ECS world owned by the editor.
///
/// Contains editor-only entities, systems, and camera render resources.
class EditorWorld {
  public:
    EditorWorld() : m_SystemScheduler(m_Registry) {
        m_Registry.ctx().emplace<entt::dispatcher>();
    }

    /// @brief Initialize editor ECS entities and systems after RHI setup.
    auto Initialize(IWindowSystem& Window) -> void {
        if (m_EditorRootEntity != entt::null)
            return;

        m_EditorRootEntity = m_Registry.create();
        m_Registry.emplace<TransformComponent>(m_EditorRootEntity);

        // EditorCameraSystem patches local transforms from input; it must run
        // before TransformSystem so the changes propagate within the same frame.
        const auto Setup = m_SystemScheduler.Register<EditorCameraSystem>(
                                   "EditorCameraSystem", {}, {}, Window)
                               .and_then([&]() -> std::expected<void, ErrorMessage> {
                                   return m_SystemScheduler.Register<TransformSystem>(
                                       "TransformSystem", {}, {"EditorCameraSystem"});
                               })
                               .and_then([&]() -> std::expected<void, ErrorMessage> {
                                   return m_SystemScheduler.CompileDependency();
                               });
        if (!Setup) {
            LogError("Editor system setup failed:\n{}", Setup.error().ToString());
            return;
        }
        m_SystemScheduler.SetupObservers();

        const auto CameraEntity = m_Registry.create();
        m_Registry.emplace<ParentComponent>(CameraEntity, m_EditorRootEntity);
        m_Registry.emplace<TransformComponent>(CameraEntity,
                                               TransformComponent{
                                                   .Translation = hlslpp::float3(1.25f, 1.25f, 2.0f),
                                                   .Rotation    = hlslpp::float3(23.46f, -35.29f, 0.0f),
                                               });
        m_Registry.emplace<NameComponent>(CameraEntity, NameComponent{.Name = "EditorViewportCamera"});
        auto& Camera = m_Registry.emplace<EditorCameraComponent>(CameraEntity);
        if (const auto Readback = RHIRenderDevice::Get().CreateReadbackBuffer(
                "Editor/Viewport/Picking", RHIReadbackBufferDesc{.Size = sizeof(Uint32)});
            Readback) {
            Camera.Readback = *Readback;
        } else {
            LogWarning("Editor viewport picking readback unavailable: {}", Readback.error().ToString());
        }

        BindWindowEvents(Window);
    }

    ~EditorWorld() {
        Shutdown();
    }

    EditorWorld(const EditorWorld&)                    = delete;
    auto operator=(const EditorWorld&) -> EditorWorld& = delete;
    EditorWorld(EditorWorld&&)                         = delete;
    auto operator=(EditorWorld&&) -> EditorWorld&      = delete;

    /// @brief Subscribe the viewport camera to window framebuffer events.
    auto BindWindowEvents(IWindowSystem& Window) -> void {
        if (m_WindowEventDispatcher)
            return;

        m_WindowEventDispatcher = &Window.GetEventDispatcher();
        m_WindowEventDispatcher->sink<FramebufferResizeEvent>().connect<&EditorWorld::OnFramebufferResize>(*this);
        // The window was created before this subscription, so its initial
        // framebuffer size did not produce a callback. Trigger it explicitly
        // to initialize the viewport camera through the same resize path.
        m_WindowEventDispatcher->trigger(FramebufferResizeEvent{
            .PreviousExtent = {},
            .CurrentExtent  = Window.GetFramebufferExtent(),
        });
    }

    /// @brief Disconnect from window framebuffer events.
    auto UnbindWindowEvents() -> void {
        if (!m_WindowEventDispatcher)
            return;

        m_WindowEventDispatcher->sink<FramebufferResizeEvent>().disconnect<&EditorWorld::OnFramebufferResize>(*this);
        m_WindowEventDispatcher = nullptr;
    }

    /// @brief Update editor ECS systems for one frame.
    auto Tick(Float32 DeltaTime) -> void {
        if (const auto Result = m_SystemScheduler.OnUpdate(DeltaTime); !Result)
            LogError("Editor system update failed:\n{}", Result.error().ToString());
        m_SystemScheduler.ClearObservers();
    }

    /// @brief Build the editor-owned Scene View render request for this frame.
    [[nodiscard]] auto BuildSnapshot() -> EditorSnapshot {
        const auto CameraSys = m_SystemScheduler.Get<EditorCameraSystem>();
        if (!CameraSys)
            return {};

        EditorSnapshot Snapshot{.Views = CameraSys->get().CollectViews()};
        for (const auto CameraEntity : m_Registry.view<EditorCameraComponent>()) {
            const auto& Camera = m_Registry.get<EditorCameraComponent>(CameraEntity);
            Snapshot.SelectedEntity = Camera.SelectedEntity;
            Snapshot.ReadbackTarget = Camera.Readback;
            const auto& IO     = ImGui::GetIO();
            if (Camera.ViewportWidth > 0 && Camera.ViewportHeight > 0 && IO.DisplaySize.x > 0.0f &&
                IO.DisplaySize.y > 0.0f && IO.MousePos.x >= 0.0f && IO.MousePos.y >= 0.0f &&
                IO.MousePos.x < IO.DisplaySize.x && IO.MousePos.y < IO.DisplaySize.y) {
                Snapshot.HoverPixel = PixelCoordinate{
                    .X = (std::min)(static_cast<Uint32>((IO.MousePos.x / IO.DisplaySize.x) * Camera.ViewportWidth),
                                    Camera.ViewportWidth - 1),
                    .Y = (std::min)(static_cast<Uint32>((IO.MousePos.y / IO.DisplaySize.y) * Camera.ViewportHeight),
                                    Camera.ViewportHeight - 1),
                };
                Snapshot.IsHovering = true;
            }
            break;
        }
        return Snapshot;
    }

    /// @brief Shut down editor ECS systems and release their owned resources.
    auto Shutdown() -> void {
        UnbindWindowEvents();

        static_cast<void>(m_SystemScheduler.Remove<EditorCameraSystem>());
        static_cast<void>(m_SystemScheduler.Remove<TransformSystem>());

        // Clearing the registry releases component-held RHI references and
        // allows EditorCameraSystem's cache to be released with the system itself.
        m_Registry.clear();

        m_EditorRootEntity = entt::null;
    }

  private:
    auto OnFramebufferResize(FramebufferResizeEvent& Event) -> void {
        auto& Dispatcher = m_Registry.ctx().get<entt::dispatcher>();
        for (const auto CameraEntity : m_Registry.view<EditorCameraComponent>()) {
            Dispatcher.enqueue<EditorCameraResizeEvent>(EditorCameraResizeEvent{
                .CameraEntity = CameraEntity,
                .Width        = static_cast<Uint32>(Event.CurrentExtent.Width),
                .Height       = static_cast<Uint32>(Event.CurrentExtent.Height),
            });
        }
    }

    entt::registry    m_Registry = {};
    SystemScheduler   m_SystemScheduler;
    entt::dispatcher* m_WindowEventDispatcher = nullptr;
    entt::entity      m_EditorRootEntity      = entt::null;
};

} // namespace SoulEngine
