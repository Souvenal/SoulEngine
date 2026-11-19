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
}

auto Editor::BindCommands(CommandDispatcher& Commands) -> std::expected<void, ErrorMessage> {
    if (m_Commands)
        return std::unexpected(ErrorMessage("Editor commands already bound"));

    std::vector<Uint64> Registrations;
    const auto Rollback = [&Commands, &Registrations](ErrorMessage Error) -> std::expected<void, ErrorMessage> {
        for (const auto Registration : Registrations)
            Commands.Retire(Registration);
        return std::unexpected(Error.Append("Failed to bind Editor commands"));
    };

    auto Panels = UIManager::Get().RegisterPanelCommands(Commands);
    if (!Panels)
        return Rollback(Panels.error());
    Registrations.insert(Registrations.end(), {Panels->Toggle, Panels->Close});

    auto Settings = RegisterSettingsCommand(Commands);
    if (!Settings)
        return Rollback(Settings.error());
    Registrations.push_back(*Settings);

    auto ApplicationCommand =
        Commands.Register(CommandDescriptor{.Name       = "editor.application.open",
                                            .Parameters = {{.Name = "application"}},
                                            .Category   = CommandCategory::Edit},
                          [this](CommandContext& Context, String Name) -> void {
                              if (const auto* Current = GetCurrentApplication(); Current && Current->GetName() == Name)
                                  return;
                              if (!ApplicationFactory::Get().Contains(Name)) {
                                  Context.Fail("stale_target",
                                               Format("Application '{}' is unavailable", Name),
                                               CommandJson{{"application", Name}});
                                  return;
                              }
                              auto Candidate = Application::Create(Name);
                              if (!Candidate) {
                                  Context.Fail("application_open_failed",
                                               Candidate.error().ToString(),
                                               CommandJson{{"application", Name}});
                                  return;
                              }
                              UnbindScene();
                              OpenApplication(std::move(*Candidate));
                              BindScene(GetCurrentApplication()->GetScene());
                          });
    if (!ApplicationCommand)
        return Rollback(ApplicationCommand.error());
    Registrations.push_back(*ApplicationCommand);

    auto RendererCommand = Commands.Register(
        CommandDescriptor{
            .Name = "editor.renderer.select", .Parameters = {{.Name = "renderer"}}, .Category = CommandCategory::Edit},
        [](CommandContext& Context, String Name) -> void {
            if (!RendererFactory::Get().Contains(Name)) {
                Context.Fail(
                    "stale_target", Format("Renderer '{}' is unavailable", Name), CommandJson{{"renderer", Name}});
                return;
            }
            if (GetCurrentRenderer() && GetCurrentRendererName() == Name)
                return;
            if (auto Selected = SelectRenderer(Name); !Selected)
                Context.Fail("renderer_selection_failed", Selected.error().ToString(), CommandJson{{"renderer", Name}});
        });
    if (!RendererCommand)
        return Rollback(RendererCommand.error());
    Registrations.push_back(*RendererCommand);

    m_Commands             = &Commands;
    m_CommandRegistrations = std::move(Registrations);
    InitializeUI(Commands);
    return {};
}
auto Editor::UnbindCommands() -> void {
    if (m_Commands) {
        for (const auto Registration : m_CommandRegistrations)
            m_Commands->Retire(Registration);
    }
    m_Commands = nullptr;
    m_CommandRegistrations.clear();
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
    UnbindCommands();
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

auto Editor::InitializeUI(CommandDispatcher& Commands) -> void {
    RegisterAllUI([this]() -> const Scene* { return m_Scene; }, Commands);
    RegisterSettingsPanel(Commands);
}

auto Editor::DrawMainMenu() -> void {
    if (!ImGui::BeginMainMenuBar())
        return;

    if (ImGui::BeginMenu("Settings")) {
        ImGui::BeginDisabled(!m_Commands);
        const bool Activated = ImGui::MenuItem("Settings", nullptr, UIManager::Get().IsUIShowing("Settings"));
        ImGui::EndDisabled();
        if (Activated) {
            if (auto Submitted = UIManager::Get().SubmitPanelToggle(*m_Commands, "Settings"); !Submitted)
                LogError("Settings command failed: {}", Submitted.error().ToString());
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Project")) {
        if (ImGui::BeginMenu("Open Application")) {
            const auto* CurrentApplication = GetCurrentApplication();
            for (const auto& Name : ApplicationFactory::Get().Keys()) {
                String     Label(Name);
                const bool IsCurrent = CurrentApplication && CurrentApplication->GetName() == Name;
                ImGui::BeginDisabled(IsCurrent || !m_Commands);
                const bool Activated = ImGui::MenuItem(Label.c_str(), nullptr, IsCurrent);
                ImGui::EndDisabled();
                if (Activated) {
                    if (auto Submitted = m_Commands->SubmitByUser("editor.application.open", String(Name)); !Submitted)
                        LogError("Application command failed: {}", Submitted.error().ToString());
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
            ImGui::BeginDisabled(IsCurrent || !m_Commands);
            const bool Activated = ImGui::MenuItem(Label.c_str(), nullptr, IsCurrent);
            ImGui::EndDisabled();
            if (Activated) {
                if (auto Submitted = m_Commands->SubmitByUser("editor.renderer.select", String(Name)); !Submitted)
                    LogError("Renderer command failed: {}", Submitted.error().ToString());
                break;
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Debug")) {
        ImGui::BeginDisabled(!m_Commands);
        const bool Activated =
            ImGui::MenuItem("Materials", nullptr, UIManager::Get().IsUIShowing("Debug.ShowMaterials"));
        ImGui::EndDisabled();
        if (Activated) {
            if (auto Submitted = UIManager::Get().SubmitPanelToggle(*m_Commands, "Debug.ShowMaterials"); !Submitted)
                LogError("Materials command failed: {}", Submitted.error().ToString());
        }
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
