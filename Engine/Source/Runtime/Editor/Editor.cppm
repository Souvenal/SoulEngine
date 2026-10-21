module;

#include <imgui.h>
#include <imgui_threaded_rendering.h>

export module Editor;

import Editor.EditorWorld;

import Core;
import EditorTypes;
import RHI;
import Scene;
import WindowSystem;

import std;

namespace SoulEngine {

/// @brief Callback that draws one UI panel with ImGui immediate-mode calls.
/// Invoked on the engine main thread between NewFrame() and Render().
export using UIPanelCallback = std::function<void()>;

/// @brief One registered UI panel in display order.
export struct UIPanel {
    String          Name;
    UIPanelCallback Callback;
};

/// @brief Editor UI subsystem: owns the ImGui context, the UI panel registry,
/// and all GPU resources needed to render ImGui draw data through the RHI command list.
///
/// Threading: the ImGui context lives on the engine main thread, where the GLFW platform backend feeds it
/// input during event polling. Each game tick BuildFrame() produces a self-owning UIDrawFrame snapshot
/// published through a latest-wins mailbox; the render thread consumes snapshots in OnRender() and emits UI
/// passes. Backend-native GPU objects (font atlas texture, dynamic buffers) are created by a one-shot
/// RHI-thread task enqueued at BindWindowSystem(); the render thread observes readiness through atomics.
export class Editor {
  public:
    Editor() = default;
    ~Editor();

    Editor(const Editor&)                    = delete;
    auto operator=(const Editor&) -> Editor& = delete;
    Editor(Editor&&)                         = delete;
    auto operator=(Editor&&) -> Editor&      = delete;

    /// @brief Initialize the Editor subsystems.
    auto Initialize() -> void;

    /// @brief Bind the active runtime Scene for read-only Editor inspection.
    /// The Editor observes this Scene and never owns it.
    auto BindScene(const Scene& SceneValue) -> void;

    /// @brief Clear the non-owning active Scene binding before Scene teardown.
    auto UnbindScene() -> void;

    /// @brief Bind the ImGui platform backend to the main window system.
    /// Must be called after Initialize(), window-system creation, and RHI device creation. Also selects the
    /// complete window-system and RHI backend combination.
    [[nodiscard]] auto BindPresentation(IWindowSystem* WindowSys, RHIRenderDevice* RenderDevice)
        -> std::expected<void, ErrorMessage>;

    /// @brief Release GPU resource refs. Must be called on the engine main
    /// thread after the render/RHI threads have joined, before RenderDevice::Destroy().
    auto ReleaseRHIResources() -> void;

    /// @brief Destroy the ImGui context after the RHI renderer backend shuts down.
    /// @brief Shut down the platform backend and destroy the owned ImGui context.
    auto Shutdown() -> void;

    /// @brief Register a UI panel. Panels run in registration order inside
    /// BuildFrame(). Main-thread only; not synchronized.
    auto RegisterPanel(String Name, UIPanelCallback Callback) -> void;

    /// @brief Update editor ECS systems for one frame.
    auto Tick(Float32 DeltaTime, ImDrawDataSnapshot& Snapshot) -> void;

    [[nodiscard]] auto BuildSnapshot() -> EditorSnapshot;

  private:
    auto InitializeImGui() -> void;
    auto InitializeUI() -> void;

    /// @brief Draw the global main menu before all Scene-dependent panels.
    auto DrawMainMenu() -> void;

    /// @brief Main-thread entry point: build the ImGui frame for this game
    /// tick and publish a draw-data snapshot for the render thread.
    auto BeginFrame(ImDrawDataSnapshot& Snapshot) -> void;

  public:
    /// @brief Render-thread entry point: consume the latest UI frame snapshot
    /// and append its draw pass to the command list. Skips silently until a frame has been published.
    auto AttachPresentationOverlay(RenderPassList& CmdList, ImDrawDataSnapshot& Snapshot) -> void;

  private:
    ImGuiContext*        m_ImGuiContext = nullptr;
    std::vector<UIPanel> m_Panels;

    ImTextureQueue m_TextureQueue;
    std::mutex     m_TextureQueueMutex;

    EditorWorld      m_EditorWorld       = {};
    // Non-owning; Launch and DrawMainMenu keep this binding in sync with the active Application and clear it
    // before the Scene is destroyed.
    const Scene*     m_Scene             = nullptr;
    // Non-owning; EngineLoop keeps the window system alive until Editor::Shutdown().
    IWindowSystem*   m_BoundWindowSystem = nullptr;
    RHIRenderDevice* m_BoundRenderDevice = nullptr;
};

} // namespace SoulEngine
