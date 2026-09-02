module;

#include <entt/entt.hpp>
#include <hlsl++.h>
#include <imgui.h>

export module Editor:EditorWorld;

import std;
import Core;
import EditorTypes;
import RHI;
import Scene;
import WindowSystem;

namespace SoulEngine {

/// @brief Editor-only state attached to an editor viewport camera entity.
struct EditorViewportComponent {
    std::optional<RenderPixelCoordinate> PendingPixel        = std::nullopt;
    std::optional<RenderPixelCoordinate> SelectedPixel       = std::nullopt;
    std::optional<entt::entity>           HoveredEntity       = std::nullopt;
    std::optional<entt::entity>           SelectedEntity      = std::nullopt;
    RHIRef<RHIReadbackBuffer>             Readback            = nullptr;
    bool                                  SelectionRequested = false;
};

class EditorCameraSystem final : public ISystem {
  public:
    /// @brief Create an editor camera system bound to one window system.
    explicit EditorCameraSystem(entt::registry& Registry, IWindowSystem& Window)
        : ISystem(Registry), m_Window(&Window) {}

    auto OnUpdate(Float32 DeltaTime) -> void override {
        UpdatePerspective(DeltaTime);
        UpdateSelection();
    }

  private:
    auto UpdatePerspective(Float32 DeltaTime) -> void {
        if (!ImGui::GetCurrentContext())
            return;
        const auto& IO = ImGui::GetIO();
        const bool CameraInputActive = !IO.WantCaptureMouse && ImGui::IsMouseDown(ImGuiMouseButton_Right);
        m_Window->SetCursorMode(CameraInputActive ? CursorMode::Disabled : CursorMode::Normal);
        if (!CameraInputActive)
            return;

        const auto        CursorDelta  = IO.MouseDelta;
        const auto        ForwardAxis  = GetAxis(ImGuiKey_W, ImGuiKey_S);
        const auto        RightAxis    = GetAxis(ImGuiKey_D, ImGuiKey_A);
        const auto        VerticalAxis = GetAxis(ImGuiKey_E, ImGuiKey_Q);
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
                Transform.Translation += Forward * (IO.MouseWheel * 0.75f);
                Transform.Rotation.y  += CursorDelta.x * Sensitivity * (180.0f / std::numbers::pi_v<Float32>);
                Transform.Rotation.x =
                    std::clamp(static_cast<Float32>(Transform.Rotation.x) +
                                   CursorDelta.y * Sensitivity * (180.0f / std::numbers::pi_v<Float32>),
                               -MaxPitch * (180.0f / std::numbers::pi_v<Float32>),
                               MaxPitch * (180.0f / std::numbers::pi_v<Float32>));
            });
        }
    }

    auto UpdateSelection() -> void {
        if (!ImGui::GetCurrentContext())
            return;
        const auto& IO = ImGui::GetIO();
        for (const auto CameraEntity : m_Registry.view<EditorViewportComponent>()) {
            auto& Viewport = m_Registry.get<EditorViewportComponent>(CameraEntity);
            if (Viewport.Readback) {
                if (const auto EntityId = Viewport.Readback->TryRead<Uint32>()) {
                    Viewport.HoveredEntity = *EntityId == GBuffer::BackgroundEntityId
                                                 ? std::nullopt
                                                 : std::optional<entt::entity>{static_cast<entt::entity>(*EntityId)};
                    if (Viewport.SelectionRequested) {
                        Viewport.SelectedEntity      = Viewport.HoveredEntity;
                        Viewport.SelectionRequested = false;
                        if (Viewport.SelectedEntity)
                            LogInfo("Editor viewport selected entity: {}",
                                    static_cast<Uint32>(*Viewport.SelectedEntity));
                    }
                }
            }
            const auto& Camera = m_Registry.get<CameraComponent>(CameraEntity);
            if (Camera.ViewportWidth == 0 || Camera.ViewportHeight == 0 || IO.DisplaySize.x <= 0.0f ||
                IO.DisplaySize.y <= 0.0f || IO.MousePos.x < 0.0f || IO.MousePos.y < 0.0f ||
                IO.MousePos.x >= IO.DisplaySize.x || IO.MousePos.y >= IO.DisplaySize.y)
                continue;
            const RenderPixelCoordinate Pixel{
                .X = (std::min)(static_cast<Uint32>((IO.MousePos.x / IO.DisplaySize.x) * Camera.ViewportWidth),
                                Camera.ViewportWidth - 1),
                .Y = (std::min)(static_cast<Uint32>((IO.MousePos.y / IO.DisplaySize.y) * Camera.ViewportHeight),
                                Camera.ViewportHeight - 1),
            };
            Viewport.PendingPixel = Pixel;
            if (!IO.WantCaptureMouse && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                Viewport.SelectionRequested = true;
                Viewport.SelectedPixel       = Pixel;
            }
        }
    }

    [[nodiscard]] auto GetAxis(ImGuiKey Positive, ImGuiKey Negative) const -> Float32 {
        return (ImGui::IsKeyDown(Positive) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(Negative) ? 1.0f : 0.0f);
    }

    IWindowSystem*     m_Window             = nullptr;
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
        if (m_EditorRootEntity != entt::null)
            return;

        m_EditorRootEntity = m_Registry.create();
        m_Registry.emplace<TransformComponent>(m_EditorRootEntity);

        // EditorCameraSystem patches local transforms from input; it must run
        // before TransformSystem so the changes propagate within the same frame.
        const auto Setup = m_SystemScheduler.Register<TransformSystem>("TransformSystem")
                               .and_then([&]() -> std::expected<void, ErrorMessage> {
                                   return m_SystemScheduler.Register<CameraSystem>("CameraSystem");
                               })
                               .and_then([&]() -> std::expected<void, ErrorMessage> {
                                   return m_SystemScheduler.Register<EditorCameraSystem>(
                                       "EditorCameraSystem", {"TransformSystem"}, {}, Window);
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
                                                   .Rotation    = hlslpp::float3(28.0f, -32.0f, 0.0f),
                                               });
        m_Registry.emplace<NameComponent>(CameraEntity, NameComponent{.Name = "EditorViewportCamera"});
        m_Registry.emplace<CameraComponent>(CameraEntity);
        auto& Viewport = m_Registry.emplace<EditorViewportComponent>(CameraEntity);
        if (const auto Readback = RHIRenderDevice::Get().CreateReadbackBuffer(
                "Editor/Viewport/Picking", RHIReadbackBufferDesc{.Size = sizeof(Uint32)});
            Readback) {
            Viewport.Readback = *Readback;
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
        const auto* CameraSys = m_SystemScheduler.Get<CameraSystem>();
        if (!CameraSys)
            return {};

        EditorSnapshot Snapshot{.Views = CameraSys->CollectViews()};
        for (const auto CameraEntity : m_Registry.view<EditorViewportComponent>()) {
            auto& Viewport = m_Registry.get<EditorViewportComponent>(CameraEntity);
            if (!Viewport.PendingPixel || !Viewport.Readback)
                continue;
            Snapshot.Picking = ScenePickingRequest{
                .Pixel = *Viewport.PendingPixel,
                .Target = Viewport.Readback,
            };
            Viewport.PendingPixel.reset();
            break;
        }
        return Snapshot;
    }

    /// @brief Return the editor viewport camera component.
    [[nodiscard]] auto GetViewportCamera() const -> const CameraComponent* {
        const auto CameraView = m_Registry.view<CameraComponent>();
        if (CameraView.empty())
            return nullptr;
        return &CameraView.get<CameraComponent>(*CameraView.begin());
    }

    /// @brief Shut down editor ECS systems and release their owned resources.
    auto Shutdown() -> void {
        UnbindWindowEvents();

        static_cast<void>(m_SystemScheduler.Remove<EditorCameraSystem>());
        static_cast<void>(m_SystemScheduler.Remove<CameraSystem>());
        static_cast<void>(m_SystemScheduler.Remove<TransformSystem>());

        // Clearing the registry releases component-held RHI references and
        // allows CameraSystem's cache to be released with the system itself.
        m_Registry.clear();

        m_EditorRootEntity = entt::null;
    }

  private:
    auto OnFramebufferResize(FramebufferResizeEvent& Event) -> void {
        auto& Dispatcher = m_Registry.ctx().get<entt::dispatcher>();
        for (const auto CameraEntity : m_Registry.view<CameraComponent>()) {
            Dispatcher.enqueue<CameraResizeEvent>(CameraResizeEvent{
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
