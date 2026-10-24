module;

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_threaded_rendering.h>

module Editor;

import magic_enum;
import Editor.UIManager;
import Editor.UIPanels;
import Editor.SettingsPanel;
import Editor.EditorWorld;

import Core;
import EditorTypes;
import Application;
import RHI;
import Renderer;
import Scene;
import WindowSystem;

import std;

namespace SoulEngine {

Editor::~Editor() {
    Shutdown();
}

auto Editor::Initialize() -> void {
    if (m_ImGuiContext) {
        LogWarning("Editor ImGui context is already created");
        return;
    }

    InitializeImGui();
    InitializeUI();
}

auto Editor::BindScene(const Scene& SceneValue) -> void {
    m_Scene = &SceneValue;
}

auto Editor::UnbindScene() -> void {
    m_Scene = nullptr;
}

auto Editor::BindPresentation(IWindowSystem* WindowSys, RHIRenderDevice* RenderDevice)
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

auto Editor::ReleaseRHIResources() -> void {
    if (m_ImGuiContext) {
        std::scoped_lock Lock(m_TextureQueueMutex);
        m_TextureQueue.Shutdown();
        m_TextureQueue.UpdateTexFunc = nullptr;
    }
    m_EditorWorld.Shutdown();
    m_BoundRenderDevice = nullptr;
}

auto Editor::Shutdown() -> void {
    if (m_BoundRenderDevice)
        ReleaseRHIResources();
    if (!m_ImGuiContext)
        return;
    ImGui::SetCurrentContext(m_ImGuiContext);
    m_BoundWindowSystem = nullptr;
    ImGui::DestroyContext(m_ImGuiContext);
    m_ImGuiContext = nullptr;
}

auto Editor::RegisterPanel(String Name, UIPanelCallback Callback) -> void {
    m_Panels.push_back({.Name = std::move(Name), .Callback = std::move(Callback)});
}

auto Editor::Tick(Float32 DeltaTime, ImDrawDataSnapshot& Snapshot) -> void {
    BeginFrame(Snapshot);
    m_EditorWorld.Tick(DeltaTime);
}

auto Editor::BuildSnapshot() -> EditorSnapshot {
    return m_EditorWorld.BuildSnapshot();
}

auto Editor::InitializeImGui() -> void {
    IMGUI_CHECKVERSION();
    m_ImGuiContext = ImGui::CreateContext();
    ImGui::SetCurrentContext(m_ImGuiContext);
    ImGui::StyleColorsDark();
}

auto Editor::InitializeUI() -> void {
    RegisterAllUI([this]() -> const Scene* { return m_Scene; });
    RegisterSettingsPanel();
}

auto Editor::DrawMainMenu() -> void {
    if (!ImGui::BeginMainMenuBar())
        return;

    if (ImGui::BeginMenu("Project")) {
        if (ImGui::BeginMenu("Open Application")) {
            const auto* CurrentApplication = GetCurrentApplication();
            for (const auto& Name : ApplicationFactory::Get().Keys()) {
                String     Label(Name);
                const bool IsCurrent = CurrentApplication && CurrentApplication->GetName() == Name;
                if (ImGui::MenuItem(Label.c_str(), nullptr, IsCurrent) && !IsCurrent) {
                    if (auto R = OpenApplication(Name); !R) {
                        LogError("Application opening failed:\n{}", R.error().ToString());
                    } else {
                        if (const auto* NewApplication = GetCurrentApplication())
                            BindScene(NewApplication->GetScene());
                        else
                            UnbindScene();
                    }
                    break;
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Render")) {
        const auto CurrentRendererName = GetCurrentRendererName();
        for (const auto& Name : RendererFactory::Get().Keys()) {
            String     Label(Name);
            const bool IsCurrent = CurrentRendererName == Name;
            if (ImGui::MenuItem(Label.c_str(), nullptr, IsCurrent) && !IsCurrent) {
                if (auto R = SelectRenderer(Name); !R)
                    LogError("Renderer selection failed:\n{}", R.error().ToString());
                break;
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Debug")) {
        if (ImGui::MenuItem("Materials", nullptr, UIManager::Get().IsUIShowing("Debug.ShowMaterials")))
            UIManager::Get().ToggleUI("Debug.ShowMaterials");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Settings")) {
        if (ImGui::MenuItem("Settings", nullptr, UIManager::Get().IsUIShowing("Settings")))
            UIManager::Get().ToggleUI("Settings");
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

auto Editor::BeginFrame(ImDrawDataSnapshot& Snapshot) -> void {
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

auto Editor::AttachPresentationOverlay(RenderPassList& CmdList, ImDrawDataSnapshot& Snapshot) -> void {
    if (!Snapshot.DrawData.Valid)
        return;

    CmdList.ImGuiPresentationOverlay = RHIImGuiPresentationOverlayCmd{
        .Snapshot     = &Snapshot,
        .TextureQueue = &m_TextureQueue,
        .TextureMutex = &m_TextureQueueMutex,
    };
}

} // namespace SoulEngine
