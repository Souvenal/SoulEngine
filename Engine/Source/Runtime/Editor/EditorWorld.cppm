module;

#include <entt/entt.hpp>
#include <hlsl++.h>
#include <imgui.h>

export module Editor:EditorWorld;

import std;
import Core;
import RHI;
import Scene;
import WindowSystem;

namespace SoulEngine {

class EditorCameraSystem final : public ISystem {
  public:
    /// @brief Create an editor camera system bound to one window system.
    ///
    /// The window system is non-owning. It is used only to subscribe to
    /// KeyboardFrameEvent and MouseFrameEvent and to switch the cursor mode
    /// between Normal and Disabled. Keyboard and mouse data are received
    /// through the frame events; this system does not query input state from
    /// the window system directly.
    explicit EditorCameraSystem(entt::registry& Registry, IWindowSystem& Window)
        : ISystem(Registry), m_Window(&Window) {}

    auto OnUpdate(Float32 DeltaTime) -> void override {
        const bool CameraInputActive = ImGui::GetCurrentContext() && !ImGui::GetIO().WantCaptureMouse &&
                                       m_MouseFrameEvent.IsButtonDown(WindowMouseButton::Right);
        m_Window->SetCursorMode(CameraInputActive ? CursorMode::Disabled : CursorMode::Normal);
        if (!CameraInputActive)
            return;

        const auto        CursorDelta  = m_MouseFrameEvent.GetCursorDelta();
        const auto        ForwardAxis  = GetAxis(WindowKey::W, WindowKey::S);
        const auto        RightAxis    = GetAxis(WindowKey::D, WindowKey::A);
        const auto        VerticalAxis = GetAxis(WindowKey::E, WindowKey::Q);
        constexpr Float32 Sensitivity  = 0.0025f;
        constexpr Float32 MaxPitch     = 1.55334306f;

        for (const auto CameraEntity : m_Registry.view<CameraComponent, TransformComponent>()) {
            const auto& CurrentTransform     = m_Registry.get<TransformComponent>(CameraEntity);
            const auto  EditorWorldTransform = CurrentTransform.GetLocalMatrix();
            const auto  WorldForward = hlslpp::mul(hlslpp::float4(0.0f, 0.0f, -1.0f, 0.0f), EditorWorldTransform);
            const auto  Forward = hlslpp::normalize(hlslpp::float3(WorldForward.x, WorldForward.y, WorldForward.z));
            const auto  HorizontalForward = hlslpp::normalize(hlslpp::float3(Forward.x, 0.0f, Forward.z));
            const auto  Up                = hlslpp::float3(0.0f, 1.0f, 0.0f);
            const auto  Right             = hlslpp::normalize(hlslpp::cross(HorizontalForward, Up));
            const auto  MoveDirection     = HorizontalForward * ForwardAxis + Right * RightAxis + Up * VerticalAxis;

            m_Registry.patch<TransformComponent>(CameraEntity, [&](auto& Transform) -> void {
                if (MoveDirection.x != 0.0f || MoveDirection.y != 0.0f || MoveDirection.z != 0.0f)
                    Transform.Translation += hlslpp::normalize(MoveDirection) * (2.0f * DeltaTime);
                Transform.Translation += Forward * (m_MouseFrameEvent.ScrollY * 0.75f);
                Transform.Rotation.y  += CursorDelta.X * Sensitivity * (180.0f / std::numbers::pi_v<Float32>);
                Transform.Rotation.x =
                    std::clamp(static_cast<Float32>(Transform.Rotation.x) +
                                   CursorDelta.Y * Sensitivity * (180.0f / std::numbers::pi_v<Float32>),
                               -MaxPitch * (180.0f / std::numbers::pi_v<Float32>),
                               MaxPitch * (180.0f / std::numbers::pi_v<Float32>));
            });
        }
    }

    auto SetupObservers() -> void override {
        auto& Dispatcher = m_Window->GetEventDispatcher();
        Dispatcher.sink<KeyboardFrameEvent>().connect<&EditorCameraSystem::OnKeyboardFrame>(*this);
        Dispatcher.sink<MouseFrameEvent>().connect<&EditorCameraSystem::OnMouseFrame>(*this);
    }

    auto TeardownObservers() -> void override {
        auto& Dispatcher = m_Window->GetEventDispatcher();
        Dispatcher.sink<KeyboardFrameEvent>().disconnect<&EditorCameraSystem::OnKeyboardFrame>(*this);
        Dispatcher.sink<MouseFrameEvent>().disconnect<&EditorCameraSystem::OnMouseFrame>(*this);
    }

  private:
    auto OnKeyboardFrame(KeyboardFrameEvent& Event) -> void {
        m_KeyboardFrameEvent = Event;
    }

    auto OnMouseFrame(MouseFrameEvent& Event) -> void {
        m_MouseFrameEvent = Event;
    }

    [[nodiscard]] auto GetAxis(WindowKey Positive, WindowKey Negative) const -> Float32 {
        return (m_KeyboardFrameEvent.IsKeyDown(Positive) ? 1.0f : 0.0f) -
               (m_KeyboardFrameEvent.IsKeyDown(Negative) ? 1.0f : 0.0f);
    }

    IWindowSystem*     m_Window             = nullptr;
    KeyboardFrameEvent m_KeyboardFrameEvent = {};
    MouseFrameEvent    m_MouseFrameEvent    = {};
};

} // namespace SoulEngine

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
        if (m_ViewportCameraEntity != entt::null)
            return;

        m_EditorRootEntity = m_Registry.create();
        m_Registry.emplace<TransformComponent>(m_EditorRootEntity);

        m_SystemScheduler.Register<TransformSystem>();
        m_SystemScheduler.Register<CameraSystem>();
        m_SystemScheduler.Register<EditorCameraSystem>(Window);
        m_SystemScheduler.SetupObservers();

        m_ViewportCameraEntity = m_Registry.create();
        m_Registry.emplace<ParentComponent>(m_ViewportCameraEntity, m_EditorRootEntity);
        m_Registry.emplace<TransformComponent>(m_ViewportCameraEntity,
                                               TransformComponent{
                                                   .Translation = hlslpp::float3(1.25f, 1.25f, 2.0f),
                                                   .Rotation    = hlslpp::float3(28.0f, -32.0f, 0.0f),
                                               });
        m_Registry.emplace<NameComponent>(m_ViewportCameraEntity, NameComponent{.Name = "EditorViewportCamera"});
        m_Registry.emplace<CameraComponent>(m_ViewportCameraEntity);

        BindWindowEvents(Window);
    }

    ~EditorWorld() {
        UnbindWindowEvents();
        m_SystemScheduler.TeardownObservers();
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
        m_SystemScheduler.OnUpdate(DeltaTime);
        m_SystemScheduler.ClearObservers();
    }

    /// @brief Build the editor-owned Scene View render request for this frame.
    [[nodiscard]] auto BuildSceneView() const -> std::optional<RenderViewSnapshot> {
        if (m_ViewportCameraEntity == entt::null || !m_Registry.valid(m_ViewportCameraEntity) ||
            !m_Registry.all_of<TransformComponent, CameraComponent>(m_ViewportCameraEntity))
            return std::nullopt;

        const auto& Transform = m_Registry.get<TransformComponent>(m_ViewportCameraEntity);
        const auto& Camera    = m_Registry.get<CameraComponent>(m_ViewportCameraEntity);
        const auto* CameraSys = m_SystemScheduler.Get<CameraSystem>();
        if (!CameraSys)
            return std::nullopt;
        return CameraSys->BuildRenderView(Camera, Transform.WorldTransform);
    }

    /// @brief Return the editor viewport camera component.
    [[nodiscard]] auto GetViewportCamera() const -> const CameraComponent* {
        if (m_ViewportCameraEntity == entt::null || !m_Registry.valid(m_ViewportCameraEntity) ||
            !m_Registry.all_of<CameraComponent>(m_ViewportCameraEntity))
            return nullptr;
        return &m_Registry.get<CameraComponent>(m_ViewportCameraEntity);
    }

    /// @brief Release camera GPU resource references before RHI shutdown.
    auto ReleaseRHIResources() -> void {
        UnbindWindowEvents();
        if (m_ViewportCameraEntity != entt::null && m_Registry.valid(m_ViewportCameraEntity)) {
            if (auto* Camera = m_Registry.try_get<CameraComponent>(m_ViewportCameraEntity)) {
                Camera->Targets        = {};
                Camera->ViewportWidth  = 0;
                Camera->ViewportHeight = 0;
            }
        }
        static_cast<void>(m_SystemScheduler.Remove<CameraSystem>());
        static_cast<void>(m_SystemScheduler.Remove<EditorCameraSystem>());
        m_ViewportCameraEntity = entt::null;
    }

  private:
    auto OnFramebufferResize(FramebufferResizeEvent& Event) -> void {
        m_Registry.ctx().get<entt::dispatcher>().enqueue<CameraResizeEvent>(CameraResizeEvent{
            .CameraEntity = m_ViewportCameraEntity,
            .Width        = static_cast<Uint32>(Event.CurrentExtent.Width),
            .Height       = static_cast<Uint32>(Event.CurrentExtent.Height),
        });
    }

    entt::registry    m_Registry = {};
    SystemScheduler   m_SystemScheduler;
    entt::dispatcher* m_WindowEventDispatcher = nullptr;
    entt::entity      m_EditorRootEntity      = entt::null;
    entt::entity      m_ViewportCameraEntity  = entt::null;
};

} // namespace SoulEngine
