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
    m_Lifecycle = EditorLifecycle::Initialized;
}

auto Editor::BindCommands(CommandDispatcher& Commands) -> std::expected<void, ErrorMessage> {
    if (m_Commands)
        return std::unexpected(ErrorMessage("Editor commands already bound"));
    auto Registered = UIManager::Get().RegisterSettingsCommand(Commands);
    if (!Registered)
        return std::unexpected(Registered.error().Append("Failed to bind Editor commands"));
    auto State = Commands.Register(
        CommandDescriptor{.Name             = "editor.state.get",
                          .IsAgentExposed   = true,
                          .Phase            = CommandPhase::PostUpdate,
                          .MaxArgumentBytes = 256,
                          .MaxResultBytes   = 65536,
                          .Description      = "Return one bounded baseline Editor state observation",
                          .Version          = 1,
                          .Category         = CommandCategory::Observe},
        [this](CommandContext& Context) -> void {
            if (!m_IsStateObservationReady || m_StateObservationFrame != Context.GetFrame()) {
                Context.Fail("observation_frame_mismatch",
                             "Editor state was not collected for this frame",
                             CommandJson{{"observationFrame",
                                          m_IsStateObservationReady ? CommandJson(m_StateObservationFrame)
                                                                    : CommandJson(nullptr)},
                                         {"completionFrame", Context.GetFrame()}});
                return;
            }
            Context.SetOutput(m_StateObservation);
        });
    if (!State) {
        Commands.Retire(*Registered);
        return std::unexpected(State.error().Append("Failed to bind Editor state command"));
    }
    m_Commands        = &Commands;
    m_SettingsCommand = *Registered;
    m_StateCommand    = *State;
    return {};
}
auto Editor::UnbindCommands() -> void {
    if (m_Commands) {
        m_Commands->Retire(m_SettingsCommand);
        m_Commands->Retire(m_StateCommand);
    }
    m_Commands                = nullptr;
    m_SettingsCommand         = 0;
    m_StateCommand            = 0;
    m_StateObservation        = nullptr;
    m_IsStateObservationReady = false;
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
    m_Lifecycle = EditorLifecycle::Running;
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
    m_Lifecycle = EditorLifecycle::Shutdown;
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

auto Editor::UpdateStateObservation(Uint64 Frame) -> void {
    UpdateStateObservation(Frame, m_EditorWorld.BuildSnapshot());
}

auto Editor::UpdateStateObservation(Uint64 Frame, EditorSnapshot Snapshot) -> void {
    const ImGuiIO* IO = nullptr;
    if (m_ImGuiContext) {
        ImGui::SetCurrentContext(m_ImGuiContext);
        IO = &ImGui::GetIO();
    }

    std::vector<String> DesiredPanels;
    if (IO) {
        for (const auto& [Name, Entry] : UIManager::Get().GetAll()) {
            if (Entry.Show)
                DesiredPanels.push_back(Name);
        }
        for (const auto& Panel : m_Panels)
            DesiredPanels.push_back(Panel.Name);
    }

    m_StateObservation        = m_StateCollector.Collect(EditorStateSource{
        .Frame            = Frame,
        .Lifecycle        = m_Lifecycle,
        .ApplicationValue = GetCurrentApplication(),
        .RendererName     = GetCurrentRendererName(),
        .SceneValue       = m_Scene,
        .Window           = m_BoundWindowSystem,
        .IO               = IO,
        .RenderBackend =
            m_BoundRenderDevice ? std::optional<RHIBackendType>(m_BoundRenderDevice->GetBackendType()) : std::nullopt,
        .EditorData    = std::move(Snapshot),
        .DesiredPanels = std::move(DesiredPanels),
        .DrawnPanels   = m_DrawnPanels,
    });
    m_StateObservationFrame   = Frame;
    m_IsStateObservationReady = true;
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

    if (ImGui::BeginMenu("Settings")) {
        if (ImGui::MenuItem("Settings", nullptr, UIManager::Get().IsUIShowing("Settings")) && m_Commands) {
            if (auto Submitted = UIManager::Get().SubmitSettingsToggle(*m_Commands); !Submitted)
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

    ImGui::EndMainMenuBar();
}

auto Editor::BeginFrame(ImDrawDataSnapshot& Snapshot) -> void {
    if (!m_ImGuiContext)
        return;
    ImGui::SetCurrentContext(m_ImGuiContext);
    m_DrawnPanels.clear();
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
            m_DrawnPanels.push_back(name);
        }
    }
    for (auto& Panel : m_Panels) {
        Panel.Callback();
        m_DrawnPanels.push_back(Panel.Name);
    }
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
