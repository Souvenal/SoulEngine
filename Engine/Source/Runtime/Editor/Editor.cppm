module;
#include <entt/entity/entity.hpp>
#include <hlsl++.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_threaded_rendering.h>

export module Editor;

import magic_enum;
import :MainMenu;
import :UIManager;
import :UIPanels;
import :EditorWorld;

import Core;
import EditorTypes;
import RHI;
import Resource;
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

    /// @brief Initialize the Editor subsystems.
    auto Initialize() -> void {
        if (m_ImGuiContext) {
            LogWarning("Editor ImGui context is already created");
            return;
        }

        InitializeImGui();
        InitializeUI();
    }

    /// @brief Bind the ImGui platform backend to the main window system.
    /// Must be called after Initialize(), window-system creation, and
    /// RHI device creation.
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
        m_EditorWorld.Initialize(*WindowSys);
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
        m_EditorWorld.Shutdown();
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

    /// @brief Update editor ECS systems for one frame.
    auto Tick(Float32 DeltaTime) -> void {
        m_EditorWorld.Tick(DeltaTime);
    }

    [[nodiscard]] auto BuildSnapshot() -> EditorSnapshot {
        return m_EditorWorld.BuildSnapshot();
    }

  private:
    auto InitializeImGui() -> void {
        IMGUI_CHECKVERSION();
        m_ImGuiContext = ImGui::CreateContext();
        ImGui::SetCurrentContext(m_ImGuiContext);
        ImGui::StyleColorsDark();
    }

    auto InitializeUI() -> void {
        RegisterAllUI();
    }

  public:
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

        // 绘制所有显示的 UI
        for (auto& [name, entry] : UIManager::Get().GetAll()) {
            if (entry.Show) {
                entry.Callback();
            }
        }
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
    /// frame has been published.
    auto AttachPresentationOverlay(RenderPassList& CmdList, ImDrawDataSnapshot& Snapshot) -> void {
        if (!Snapshot.DrawData.Valid)
            return;

        CmdList.ImGuiPresentationOverlay = RHIImGuiPresentationOverlayCmd{
            .Snapshot     = &Snapshot,
            .TextureQueue = &m_TextureQueue,
            .TextureMutex = &m_TextureQueueMutex,
        };
    }

  private:
    ImGuiContext*        m_ImGuiContext = nullptr;
    std::vector<UIPanel> m_Panels;

    ImTextureQueue m_TextureQueue;
    std::mutex     m_TextureQueueMutex;

    EditorWorld m_EditorWorld = {};
    // Non-owning; EngineLoop keeps the window system alive until Editor::Shutdown().
    IWindowSystem*                       m_BoundWindowSystem = nullptr;
    RHIRenderDevice*                     m_BoundRenderDevice = nullptr;
};

} // namespace SoulEngine
