module;
#include <hlsl++.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_threaded_rendering.h>
#include <magic_enum/magic_enum.hpp>

export module Editor;

import :MainMenu;

import Core;
import RHI;
import Scene;
import WindowSystem;

export import std;

export namespace SoulEngine {

/// @brief Callback that draws one UI panel with ImGui immediate-mode calls.
/// Invoked on the engine main thread between NewFrame() and Render().
using UIPanelCallback = std::function<void()>;

/// @brief One registered UI panel in display order.
struct UIPanel {
    String          Name;
    UIPanelCallback Callback;
};

/// @brief Editor UI subsystem: owns the ImGui context, the UI panel registry,
/// and all GPU resources needed to render ImGui draw data through the RHI
/// command list.
///
/// Threading: the ImGui context lives on the engine main thread, where the
/// GLFW platform backend feeds it input during event polling. Each game tick
/// BuildFrame() produces a self-owning UIDrawFrame snapshot published through
/// a latest-wins mailbox; the render thread consumes snapshots in OnRender()
/// and emits UI passes. Backend-native GPU objects (font atlas texture,
/// dynamic buffers) are created by a one-shot RHI-thread task enqueued at
/// BindWindowSystem(); the render thread observes readiness through atomics.
class Editor {
  public:
    Editor() = default;
    ~Editor() {
        Shutdown();
    }

    Editor(const Editor&)                    = delete;
    auto operator=(const Editor&) -> Editor& = delete;
    Editor(Editor&&)                         = delete;
    auto operator=(Editor&&) -> Editor&      = delete;

    /// @brief Create the owned Dear ImGui context.
    [[nodiscard]] auto Create() -> std::expected<void, ErrorMessage> {
        if (m_ImGuiContext)
            return std::unexpected(ErrorMessage("Editor ImGui context is already created"));

        IMGUI_CHECKVERSION();
        m_ImGuiContext = ImGui::CreateContext();
        if (!m_ImGuiContext)
            return std::unexpected(ErrorMessage("Editor ImGui::CreateContext failed"));

        ImGui::SetCurrentContext(m_ImGuiContext);
        ImGui::StyleColorsDark();
        return {};
    }

    /// @brief Bind the ImGui platform backend to the main window system.
    /// Must be called after Create() and ResourceManager::Init().
    /// Also selects the complete window-system and RHI backend combination.
    [[nodiscard]] auto BindPresentation(IWindowSystem* WindowSys, RHIRenderDevice* RenderDevice)
        -> std::expected<void, ErrorMessage> {
        if (!m_ImGuiContext)
            return std::unexpected(ErrorMessage("Editor ImGui context is null"));
        if (m_BoundWindowSystem)
            return std::unexpected(ErrorMessage("Editor window system is already bound"));
        if (m_BoundRenderDevice)
            return std::unexpected(ErrorMessage("Editor render device is already bound"));

        if (!WindowSys || !WindowSys->IsValid())
            return std::unexpected(ErrorMessage("Editor requires a valid window system"));
        if (!RenderDevice)
            return std::unexpected(ErrorMessage("Editor requires a render device"));

        const auto RHIType = RenderDevice->GetBackendType();
        switch (RHIType) {
        case RHIBackendType::Vulkan:
            m_TextureQueue.UpdateTexFunc = ImGui_ImplVulkan_UpdateTexture;
            break;
        case RHIBackendType::Unknown:
            return std::unexpected(
                ErrorMessage(Format("Editor cannot use RHI backend type '{}'", magic_enum::enum_name(RHIType))));
        default:
            return std::unexpected(
                ErrorMessage(Format("Editor does not support RHI backend type '{}'", magic_enum::enum_name(RHIType))));
        }

        m_BoundWindowSystem = WindowSys;
        m_BoundRenderDevice = RenderDevice;
        return {};
    }

    /// @brief Release GPU resource refs. Must be called on the engine main
    /// thread after the render/RHI threads have joined, before
    /// ResourceManager::Clear() and RenderDevice::Destroy().
    auto ReleaseRHIResources() -> void {
        if (m_ImGuiContext) {
            std::scoped_lock Lock(m_TextureQueueMutex);
            m_TextureQueue.Shutdown();
            m_TextureQueue.UpdateTexFunc = nullptr;
        }
        m_SceneViewCamera = {};
        m_BoundRenderDevice = nullptr;
    }

    /// @brief Destroy the ImGui context after the RHI renderer backend shuts down.
    /// @brief Shut down the platform backend and destroy the owned ImGui context.
    auto Shutdown() -> void {
        if (m_BoundRenderDevice)
            ReleaseRHIResources();
        if (!m_ImGuiContext)
            return;
        ImGui::SetCurrentContext(m_ImGuiContext);
        m_BoundWindowSystem = nullptr;
        ImGui::DestroyContext(m_ImGuiContext);
        m_ImGuiContext = nullptr;
    }

    /// @brief Register a UI panel. Panels run in registration order inside
    /// BuildFrame(). Main-thread only; not synchronized.
    auto RegisterPanel(String Name, UIPanelCallback Callback) -> void {
        m_Panels.push_back({.Name = std::move(Name), .Callback = std::move(Callback)});
    }

    /// @brief Resize the editor-owned Scene View output resources.
    auto ResizeSceneViewport(Uint32 Width, Uint32 Height) -> void {
        m_SceneViewCamera.ResizeViewport("editor_scene_view", Width, Height);
    }

    /// @brief Apply focused right-mouse navigation to the editor-local camera.
    auto UpdateSceneCamera(float DeltaTime, IWindowSystem& Window) -> void {
        if (!m_ImGuiContext)
            return;

        ImGui::SetCurrentContext(m_ImGuiContext);
        const bool CameraInputActive =
            !ImGui::GetIO().WantCaptureMouse && Window.IsMouseButtonPressed(WindowMouseButton::Right);
        Window.SetCursorCaptured(CameraInputActive);
        if (!CameraInputActive) {
            static_cast<void>(Window.ConsumeScrollDelta());
            static_cast<void>(Window.ConsumeCursorDelta());
            return;
        }

        const float ForwardInput = (Window.IsKeyPressed(WindowKey::W) ? 1.0f : 0.0f) -
                                   (Window.IsKeyPressed(WindowKey::S) ? 1.0f : 0.0f);
        const float RightInput = (Window.IsKeyPressed(WindowKey::D) ? 1.0f : 0.0f) -
                                 (Window.IsKeyPressed(WindowKey::A) ? 1.0f : 0.0f);
        const float VerticalInput = (Window.IsKeyPressed(WindowKey::E) ? 1.0f : 0.0f) -
                                    (Window.IsKeyPressed(WindowKey::Q) ? 1.0f : 0.0f);
        const auto EditorTransform = GetSceneViewWorldTransform();
        const auto Forward = m_SceneViewCamera.GetForward(EditorTransform);
        const auto HorizontalForward = hlslpp::normalize(hlslpp::float3(Forward.x, 0.0f, Forward.z));
        const auto Up = hlslpp::float3(0.0f, 1.0f, 0.0f);
        const auto Right = hlslpp::normalize(hlslpp::cross(HorizontalForward, Up));
        const auto MoveDirection = HorizontalForward * ForwardInput + Right * RightInput + Up * VerticalInput;
        if (MoveDirection.x != 0.0f || MoveDirection.y != 0.0f || MoveDirection.z != 0.0f)
            m_SceneViewTransform.Translation += hlslpp::normalize(MoveDirection) * (2.0f * DeltaTime);
        m_SceneViewTransform.Translation += Forward * (Window.ConsumeScrollDelta() * 0.75f);

        constexpr float Sensitivity = 0.0025f;
        constexpr float MaxPitch    = 1.55334306f;
        const auto CursorDelta = Window.ConsumeCursorDelta();
        m_SceneViewTransform.Rotation.y += CursorDelta.X * Sensitivity * (180.0f / std::numbers::pi_v<float>);
        m_SceneViewTransform.Rotation.x = std::clamp(
            static_cast<float>(m_SceneViewTransform.Rotation.x) +
                CursorDelta.Y * Sensitivity * (180.0f / std::numbers::pi_v<float>),
            -MaxPitch * (180.0f / std::numbers::pi_v<float>),
            MaxPitch * (180.0f / std::numbers::pi_v<float>));
    }

    /// @brief Build the editor-owned Scene View render request for this frame.
    [[nodiscard]] auto BuildSceneView() const -> std::optional<RenderViewSnapshot> {
        return m_SceneViewCamera.BuildRenderView(GetSceneViewWorldTransform());
    }

    /// @brief Main-thread entry point: build the ImGui frame for this game
    /// tick and publish a draw-data snapshot for the render thread.
    auto BeginFrame(ImDrawDataSnapshot& Snapshot) -> void {
        if (!m_ImGuiContext)
            return;
        ImGui::SetCurrentContext(m_ImGuiContext);
        {
            std::scoped_lock Lock(m_TextureQueueMutex);
            m_TextureQueue.PreNewFrame();
        }
        if (m_BoundWindowSystem) {
            switch (m_BoundWindowSystem->GetType()) {
            case WindowSystemType::Glfw:
                ImGui_ImplGlfw_NewFrame();
                break;
            default:
                break;
            }
        }
        if (m_BoundRenderDevice) {
            switch (m_BoundRenderDevice->GetBackendType()) {
            case RHIBackendType::Vulkan:
                ImGui_ImplVulkan_NewFrame();
                break;
            default:
                break;
            }
        }
        ImGui::NewFrame();
        DrawMainMenu();
        for (auto& Panel : m_Panels)
            Panel.Callback();
        ImGui::Render();
        ImDrawData* DrawData = ImGui::GetDrawData();
        if (!DrawData || !DrawData->Valid)
            return;

        {
            std::scoped_lock Lock(m_TextureQueueMutex);
            m_TextureQueue.QueueRequests(DrawData);
        }
        Snapshot.SnapUsingSwap(DrawData, ImGui::GetTime());
    }

    /// @brief Render-thread entry point: consume the latest UI frame snapshot
    /// and append its draw pass to the command list. Skips silently until a
    /// frame has been published and the pipeline, font texture, and dynamic
    /// buffers are all ready.
    auto AttachPresentationOverlay(RHICommandList& CmdList, ImDrawDataSnapshot& Snapshot) -> void {
        if (!CmdList.PresentSource)
            return;

        if (!Snapshot.DrawData.Valid)
            return;

        CmdList.ImGuiPresentationOverlay = RHIImGuiPresentationOverlayCmd{
            .Snapshot     = &Snapshot,
            .TextureQueue = &m_TextureQueue,
            .TextureMutex = &m_TextureQueueMutex,
        };
    }

  private:
    [[nodiscard]] auto GetSceneViewWorldTransform() const -> Transform {
        auto Result = m_SceneViewTransform;
        Result.WorldTransform = Result.GetLocalMatrix();
        return Result;
    }

    ImGuiContext*          m_ImGuiContext = nullptr;
    std::vector<UIPanel>   m_Panels;

    ImTextureQueue m_TextureQueue;
    std::mutex      m_TextureQueueMutex;

    Camera m_SceneViewCamera = {};
    Transform m_SceneViewTransform{
        .Translation = hlslpp::float3(1.25f, 1.25f, 2.0f),
        .Rotation = hlslpp::float3(28.0f, -32.0f, 0.0f),
    };
    // Non-owning; EngineLoop keeps the window system alive until Editor::Shutdown().
    IWindowSystem* m_BoundWindowSystem = nullptr;
    RHIRenderDevice* m_BoundRenderDevice = nullptr;
};

} // namespace SoulEngine
