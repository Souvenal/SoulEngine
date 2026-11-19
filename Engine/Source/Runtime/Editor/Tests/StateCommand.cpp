#include <entt/entt.hpp>
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_threaded_rendering.h>
#include <nlohmann/json.hpp>

import std;
import Application;
import Core;
import Editor;
import Editor.EditorCamera;
import Editor.UIManager;
import EditorCommands;
import EditorTypes;
import RHI;
import Renderer;
import Scene;
import WindowSystem;

using namespace SoulEngine;
using Json = nlohmann::json;

namespace SoulEngine::EditorStateTests {

struct TestWindow final : public IWindowSystem {
    FramebufferExtent Extent = {};

    [[nodiscard]] auto GetType() const -> WindowSystemType override {
        return WindowSystemType::Unknown;
    }
    [[nodiscard]] auto IsValid() const -> bool override {
        return true;
    }
    auto               Shutdown() -> void override {}
    [[nodiscard]] auto Tick() -> bool override {
        return false;
    }
    [[nodiscard]] auto GetEventDispatcher() -> entt::dispatcher& override {
        return m_EventDispatcher;
    }
    [[nodiscard]] auto GetFramebufferExtent() const -> FramebufferExtent override {
        return Extent;
    }
    auto SetCursorMode(CursorMode) -> void override {}
};

class TestApplication final : public Application {
  public:
    TestApplication() {
        m_Name  = "CollectorApp";
        m_Scene = std::make_unique<Scene>();
    }
};

class ImGuiScope {
  public:
    ImGuiScope() {
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
    }
    ~ImGuiScope() {
        ImGui::DestroyContext();
    }
};

struct FakeReadback final : public RHIReadback {
    FakeReadback() : RHIReadback("EditorStateTest", sizeof(Uint32)) {}

    [[nodiscard]] auto TryRead() const -> std::vector<std::byte> override {
        ++Reads;
        return Payload;
    }

    std::vector<std::byte> Payload = {};
    mutable Uint32         Reads   = 0;
};

class ReadbackResources {
  public:
    ReadbackResources() : m_Previous(GDeferredDeletionQueue) {
        GDeferredDeletionQueue = &m_DeletionQueue;
    }
    ~ReadbackResources() {
        m_DeletionQueue.Drain();
        GDeferredDeletionQueue = m_Previous;
    }

  private:
    RHIDeferredDeletionQueue  m_DeletionQueue = {};
    RHIDeferredDeletionQueue* m_Previous      = nullptr;
};

[[nodiscard]] auto EntityPayload(Uint32 Entity) -> std::vector<std::byte> {
    std::vector<std::byte> Payload(sizeof(Entity));
    std::memcpy(Payload.data(), &Entity, sizeof(Entity));
    return Payload;
}

[[nodiscard]] auto HasPanel(const Json& List, StringView Name) -> bool {
    return std::ranges::any_of(List["items"],
                               [&](const auto& Item) -> bool { return Item["name"].template get<String>() == Name; });
}

} // namespace SoulEngine::EditorStateTests

using namespace SoulEngine::EditorStateTests;

TEST(EditorStateCommand, RegisteredEmptyArgumentQueryUsesMatchingPostUpdateFrame) {
    CommandDispatcher Commands;
    Editor            EditorValue;
    EditorValue.Initialize();
    ASSERT_TRUE(EditorValue.BindCommands(Commands));

    const auto Exposed = Commands.DescribeExposed();
    const auto Found   = std::ranges::find(Exposed, "editor.state.get", &CommandDescriptor::Name);
    ASSERT_NE(Found, Exposed.end());
    EXPECT_TRUE(Found->IsAgentExposed);
    EXPECT_EQ(Found->Phase, CommandPhase::PostUpdate);
    EXPECT_EQ(Found->Category, CommandCategory::Observe);
    EXPECT_EQ(Found->Version, 1);
    EXPECT_EQ(Found->MaxArgumentBytes, 256);
    EXPECT_EQ(Found->MaxResultBytes, 65536);
    EXPECT_TRUE(Found->Parameters.empty());
    EXPECT_EQ(Found->Schema["additionalProperties"], false);

    std::vector<CommandResult> Results;
    ASSERT_TRUE(Commands.SubmitByAgent("editor.state.get", Json::object(), [&](CommandResult Result) -> void {
        Results.push_back(std::move(Result));
    }));
    EXPECT_TRUE(Results.empty());
    EditorValue.UpdateStateObservation(73);
    Commands.ExecutePhase(CommandPhase::PostUpdate, 73);
    ASSERT_EQ(Results.size(), 1);
    EXPECT_EQ(Results[0].Status, CommandStatus::Succeeded);
    ASSERT_TRUE(Results[0].Frame);
    EXPECT_EQ(*Results[0].Frame, 73);
    EXPECT_EQ(Results[0].Output["schemaVersion"], 1);
    EXPECT_EQ(Results[0].Output["frame"], 73);
    EXPECT_EQ(Results[0].Output["phase"], "post_update");
    EXPECT_EQ(Results[0].Output["application"]["status"], "unavailable");
    EXPECT_EQ(Results[0].Output["viewport"]["status"], "unavailable");

    std::vector<CommandResult> Invalid;
    EXPECT_FALSE(Commands.SubmitByAgent("editor.state.get", Json{{"unexpected", 1}}, [&](CommandResult Result) -> void {
        Invalid.push_back(std::move(Result));
    }));
    ASSERT_EQ(Invalid.size(), 1);
    ASSERT_TRUE(Invalid[0].Failure);
    EXPECT_EQ(Invalid[0].Failure->Code, "invalid_arguments");

    ASSERT_TRUE(Commands.SubmitByAgent("editor.state.get", Json::object(), [&](CommandResult Result) -> void {
        Results.push_back(std::move(Result));
    }));
    Commands.ExecutePhase(CommandPhase::PostUpdate, 74);
    ASSERT_EQ(Results.size(), 2);
    EXPECT_EQ(Results[1].Status, CommandStatus::Failed);
    ASSERT_TRUE(Results[1].Failure);
    EXPECT_EQ(Results[1].Failure->Code, "observation_frame_mismatch");
    EXPECT_EQ(Results[1].Failure->Details["completionFrame"], 74);

    EditorValue.UnbindCommands();
    EditorValue.Shutdown();
}

TEST(EditorStateCommand, ActionPhaseDesiredVisibilityDiffersFromPreviouslyDrawnPanels) {
    CommandDispatcher Commands;
    Editor            EditorValue;
    EditorValue.Initialize();
    ASSERT_TRUE(EditorValue.BindCommands(Commands));
    auto& UI = UIManager::Get();
    UI.CloseUI("Settings");

    auto& IO                   = ImGui::GetIO();
    IO.IniFilename             = nullptr;
    IO.DisplaySize             = ImVec2(800, 600);
    IO.DisplayFramebufferScale = ImVec2(1, 1);
    IO.DeltaTime               = 1.0F / 60.0F;
    unsigned char* Pixels      = nullptr;
    int            Width       = 0;
    int            Height      = 0;
    IO.Fonts->GetTexDataAsRGBA32(&Pixels, &Width, &Height);
    IO.Fonts->SetTexID(ImTextureID(1));
    IO.Fonts->TexData->SetStatus(ImTextureStatus_OK);
    auto BuildUiFrame = [&]() -> void {
        ImDrawDataSnapshot Snapshot;
        EditorValue.Tick(1.0F / 60.0F, Snapshot);
    };
    BuildUiFrame();

    ASSERT_TRUE(UI.SubmitSettingsToggle(Commands));
    Commands.ExecutePhase(CommandPhase::Action, 1);
    EXPECT_TRUE(UI.IsUIShowing("Settings"));
    EditorValue.UpdateStateObservation(1);
    std::vector<CommandResult> Results;
    ASSERT_TRUE(Commands.SubmitByAgent("editor.state.get", Json::object(), [&](CommandResult Result) -> void {
        Results.push_back(std::move(Result));
    }));
    Commands.ExecutePhase(CommandPhase::PostUpdate, 1);
    ASSERT_EQ(Results.size(), 1);
    ASSERT_EQ(Results[0].Status, CommandStatus::Succeeded);
    EXPECT_TRUE(HasPanel(Results[0].Output["panels"]["desired"], "Settings"));
    EXPECT_FALSE(HasPanel(Results[0].Output["panels"]["drawn"], "Settings"));

    BuildUiFrame();
    EditorValue.UpdateStateObservation(2);
    ASSERT_TRUE(Commands.SubmitByAgent("editor.state.get", Json::object(), [&](CommandResult Result) -> void {
        Results.push_back(std::move(Result));
    }));
    Commands.ExecutePhase(CommandPhase::PostUpdate, 2);
    ASSERT_EQ(Results.size(), 2);
    EXPECT_TRUE(HasPanel(Results[1].Output["panels"]["desired"], "Settings"));
    EXPECT_TRUE(HasPanel(Results[1].Output["panels"]["drawn"], "Settings"));

    UI.CloseUI("Settings");
    EditorValue.UnbindCommands();
    EditorValue.Shutdown();
}

TEST(EditorStateCollector, NormalObservationMapsCoordinatesAndSelection) {
    ImGuiScope ImGui;
    auto&      IO              = ImGui::GetIO();
    IO.DisplaySize             = ImVec2(400, 200);
    IO.DisplayFramebufferScale = ImVec2(2, 2);
    IO.AppFocusLost            = false;
    IO.WantCaptureMouse        = true;
    IO.WantCaptureKeyboard     = false;
    IO.WantTextInput           = true;

    TestWindow Window;
    Window.Extent = {.Width = 800, .Height = 400};
    TestApplication ApplicationValue;
    const auto      Entity   = ApplicationValue.GetScene().CreateEntity();
    EditorSnapshot  Snapshot = {};
    Snapshot.PrimaryViewport = CameraViewport{.X = 20, .Y = 10, .Width = 200, .Height = 100};
    Snapshot.SelectedEntity  = Entity;
    Snapshot.ReadbackEntity  = Entity;
    Snapshot.ReadbackState   = EditorReadbackState::LatestKnown;
    Snapshot.IsHovering      = true;
    Snapshot.HoverPixel      = {.X = 5, .Y = 6};

    const auto Output = EditorStateCollector{}.Collect(EditorStateSource{
        .Frame            = 42,
        .Lifecycle        = EditorLifecycle::Running,
        .ApplicationValue = &ApplicationValue,
        .RendererName     = "Raster",
        .SceneValue       = &ApplicationValue.GetScene(),
        .Window           = &Window,
        .IO               = &IO,
        .RenderBackend    = RHIBackendType::Vulkan,
        .EditorData       = std::move(Snapshot),
        .DesiredPanels    = {"Zulu", "Alpha"},
        .DrawnPanels      = {"Zulu"},
    });

    EXPECT_EQ(Output["lifecycle"], "running");
    EXPECT_EQ(Output["application"]["name"], "CollectorApp");
    EXPECT_EQ(Output["renderer"]["name"], "Raster");
    EXPECT_EQ(Output["renderer"]["backend"], "Vulkan");
    EXPECT_EQ(Output["window"]["logical"]["width"], 400);
    EXPECT_EQ(Output["window"]["physical"]["width"], 800);
    EXPECT_EQ(Output["window"]["framebufferScale"]["x"], 2.0);
    EXPECT_EQ(Output["viewport"]["physical"]["x"], 20.0);
    EXPECT_EQ(Output["viewport"]["logical"]["x"], 10.0);
    EXPECT_EQ(Output["viewport"]["logical"]["width"], 100.0);
    EXPECT_TRUE(Output["viewport"]["isDrawable"]);
    EXPECT_TRUE(Output["viewport"]["hover"]["isHovering"]);
    EXPECT_EQ(Output["viewport"]["hover"]["localPhysical"]["x"], 5);
    EXPECT_TRUE(Output["input"]["focus"]["isFocused"]);
    EXPECT_TRUE(Output["input"]["capture"]["mouse"]);
    EXPECT_FALSE(Output["input"]["capture"]["keyboard"]);
    EXPECT_EQ(Output["input"]["ownership"]["status"], "unavailable");
    EXPECT_EQ(Output["panels"]["desired"]["items"][0]["name"], "Alpha");
    EXPECT_EQ(Output["panels"]["desired"]["total"], 2);
    EXPECT_EQ(Output["panels"]["drawn"]["total"], 1);
    EXPECT_EQ(Output["selection"]["status"], "available");
    EXPECT_EQ(Output["selection"]["entity"], entt::to_integral(Entity));
    EXPECT_EQ(Output["selection"]["readback"]["status"], "latest_known");
    EXPECT_EQ(Output["selection"]["readback"]["latestKnown"]["entity"], entt::to_integral(Entity));
    EXPECT_TRUE(Output["selection"]["readback"]["latestKnown"]["sourceFrame"].is_null());
    EXPECT_TRUE(Output["selection"]["readback"]["latestKnown"]["isValidInScene"]);
    EXPECT_LE(Output.dump().size(), 65536);
}

TEST(EditorStateCollector, MissingContextsAndZeroSizesStayExplicit) {
    const auto Missing = EditorStateCollector{}.Collect(EditorStateSource{.Frame = 9});
    EXPECT_EQ(Missing["application"]["status"], "unavailable");
    EXPECT_EQ(Missing["renderer"]["status"], "unavailable");
    EXPECT_EQ(Missing["window"]["logical"]["status"], "unavailable");
    EXPECT_EQ(Missing["window"]["physical"]["status"], "unavailable");
    EXPECT_EQ(Missing["input"]["focus"]["status"], "unavailable");
    EXPECT_EQ(Missing["panels"]["drawn"]["status"], "unavailable");
    EXPECT_EQ(Missing["selection"]["reason"], "scene_context_unavailable");
    EXPECT_EQ(Missing["selection"]["readback"]["status"], "unavailable");

    EditorSnapshot MissingImGuiSnapshot  = {};
    MissingImGuiSnapshot.PrimaryViewport = CameraViewport{.Width = 10, .Height = 10};
    MissingImGuiSnapshot.IsHovering      = true;
    MissingImGuiSnapshot.HoverPixel      = {.X = 2, .Y = 3};
    const auto MissingImGui =
        EditorStateCollector{}.Collect(EditorStateSource{.Frame = 9, .EditorData = std::move(MissingImGuiSnapshot)});
    EXPECT_EQ(MissingImGui["viewport"]["hover"]["status"], "unavailable");
    EXPECT_TRUE(MissingImGui["viewport"]["hover"]["localPhysical"].is_null());

    ImGuiScope ImGui;
    auto&      IO              = ImGui::GetIO();
    IO.DisplaySize             = ImVec2(0, 0);
    IO.DisplayFramebufferScale = ImVec2(1, 1);
    TestWindow Window;
    Window.Extent            = {.Width = 0, .Height = 0};
    EditorSnapshot Snapshot  = {};
    Snapshot.PrimaryViewport = CameraViewport{};
    Snapshot.ReadbackState   = EditorReadbackState::Unavailable;
    Snapshot.ReadbackReason  = "no_completed_readback";
    const auto Zero          = EditorStateCollector{}.Collect(EditorStateSource{
        .Frame      = 10,
        .Lifecycle  = EditorLifecycle::Running,
        .Window     = &Window,
        .IO         = &IO,
        .EditorData = std::move(Snapshot),
    });
    EXPECT_EQ(Zero["window"]["physical"]["status"], "available");
    EXPECT_EQ(Zero["window"]["physical"]["width"], 0);
    EXPECT_EQ(Zero["window"]["physical"]["height"], 0);
    EXPECT_EQ(Zero["viewport"]["status"], "available");
    EXPECT_FALSE(Zero["viewport"]["isDrawable"]);
    EXPECT_EQ(Zero["viewport"]["hover"]["status"], "unavailable");
    EXPECT_EQ(Zero["viewport"]["hover"]["reason"], "zero_sized_viewport");
    EXPECT_FALSE(Zero["selection"]["readback"]["isPending"]);
}

TEST(EditorStateCollector, PanelListsAreSortedUniqueAndUtf8Bounded) {
    ImGuiScope ImGui;
    auto&      IO              = ImGui::GetIO();
    IO.DisplaySize             = ImVec2(640, 480);
    IO.DisplayFramebufferScale = ImVec2(1, 1);

    std::vector<String> Names;
    for (int Index = 69; Index >= 0; --Index)
        Names.push_back(Format("panel-{:03}", Index));
    Names.push_back("panel-000");
    const auto  Output  = EditorStateCollector{}.Collect(EditorStateSource{
        .Frame         = 11,
        .IO            = &IO,
        .DesiredPanels = std::move(Names),
        .DrawnPanels   = {String(300, 'x'), String(255, 'a') + "é" + "b"},
    });
    const auto& Desired = Output["panels"]["desired"];
    ASSERT_EQ(Desired["items"].size(), 64);
    EXPECT_EQ(Desired["total"], 70);
    EXPECT_EQ(Desired["omitted"], 6);
    EXPECT_TRUE(Desired["isTruncated"]);
    EXPECT_EQ(Desired["items"][0]["name"], "panel-000");
    EXPECT_EQ(Desired["items"][63]["name"], "panel-063");

    const auto& Drawn = Output["panels"]["drawn"];
    ASSERT_EQ(Drawn["items"].size(), 2);
    EXPECT_EQ(Drawn["items"][0]["name"].get<String>().size(), 255);
    EXPECT_EQ(Drawn["items"][1]["name"].get<String>().size(), 256);
    EXPECT_TRUE(Drawn["items"][0]["isTruncated"]);
    EXPECT_TRUE(Drawn["items"][1]["isTruncated"]);
    EXPECT_FALSE(Output.dump().empty());

    const auto  Collision = EditorStateCollector{}.Collect(EditorStateSource{
        .Frame         = 12,
        .IO            = &IO,
        .DesiredPanels = {String(256, 'c') + "a", String(256, 'c') + "b"},
    });
    const auto& Coalesced = Collision["panels"]["desired"];
    ASSERT_EQ(Coalesced["items"].size(), 1);
    EXPECT_EQ(Coalesced["total"], 2);
    EXPECT_EQ(Coalesced["omitted"], 0);
    EXPECT_EQ(Coalesced["collisions"], 1);
    EXPECT_EQ(Coalesced["items"][0]["name"].get<String>().size(), 256);
    EXPECT_TRUE(Coalesced["isTruncated"]);
}

TEST(EditorStateReadback, CameraTryReadPathsFeedThePublicCommandWithoutBlocking) {
    ReadbackResources Resources;
    CommandDispatcher Commands;
    Editor            EditorValue;
    EditorValue.Initialize();
    ASSERT_TRUE(EditorValue.BindCommands(Commands));
    auto& IO                   = ImGui::GetIO();
    IO.DisplaySize             = ImVec2(64, 64);
    IO.DisplayFramebufferScale = ImVec2(1, 1);
    IO.MousePos                = ImVec2(8, 8);
    IO.WantCaptureMouse        = true;

    TestWindow     Window;
    entt::registry Registry;
    Registry.ctx().emplace<entt::dispatcher>();
    EditorCameraSystem CameraSystem(Registry, Window);
    const auto         CameraEntity = Registry.create();
    Registry.emplace<TransformComponent>(CameraEntity);
    auto& Camera    = Registry.emplace<EditorCameraComponent>(CameraEntity);
    Camera.Viewport = {.Width = 64, .Height = 64};

    auto Readback = RHIRef<RHIReadback>::Create();
    ASSERT_TRUE(Readback.Publish(std::make_unique<FakeReadback>(), RHIRefState::Ready));
    auto* Fake = static_cast<FakeReadback*>(Readback.TryGet());
    ASSERT_NE(Fake, nullptr);
    Camera.Readback = Readback;

    Uint64 NextFrame = 1;
    auto   Invoke    = [&](EditorSnapshot Snapshot) -> CommandResult {
        const Uint64 Frame = NextFrame++;
        EditorValue.UpdateStateObservation(Frame, std::move(Snapshot));
        CommandResult Result = {};
        auto          Submitted =
            Commands.SubmitByAgent("editor.state.get", Json::object(), [&](CommandResult Completed) -> void {
                Result = std::move(Completed);
            });
        EXPECT_TRUE(Submitted);
        EXPECT_EQ(Result.Status, CommandStatus::Unknown);
        Commands.ExecutePhase(CommandPhase::PostUpdate, Frame);
        EXPECT_TRUE(Result.Frame);
        EXPECT_EQ(Result.Output["frame"], Frame);
        return Result;
    };

    CameraSystem.OnUpdate(1.0F / 60.0F);
    auto Result = Invoke(CameraSystem.BuildSnapshot());
    ASSERT_EQ(Result.Status, CommandStatus::Succeeded);
    EXPECT_EQ(Result.Output["selection"]["readback"]["status"], "unavailable");
    EXPECT_EQ(Result.Output["selection"]["readback"]["reason"], "no_completed_readback");
    EXPECT_EQ(Fake->Reads, 1);

    CameraSystem.OnUpdate(1.0F / 60.0F);
    Result = Invoke(CameraSystem.BuildSnapshot());
    EXPECT_EQ(Result.Output["selection"]["readback"]["status"], "pending");
    EXPECT_EQ(Result.Output["selection"]["readback"]["reason"], "scheduled_readback_incomplete");
    EXPECT_TRUE(Result.Output["selection"]["readback"]["isPending"]);
    EXPECT_EQ(Fake->Reads, 2);

    Camera.IsReadbackScheduled = false;
    Fake->Payload              = EntityPayload(17);
    CameraSystem.OnUpdate(1.0F / 60.0F);
    Result = Invoke(CameraSystem.BuildSnapshot());
    EXPECT_EQ(Result.Output["selection"]["readback"]["status"], "latest_known");
    EXPECT_EQ(Result.Output["selection"]["readback"]["latestKnown"]["entity"], 17);
    EXPECT_TRUE(Result.Output["selection"]["readback"]["latestKnown"]["sourceFrame"].is_null());

    Fake->Payload = EntityPayload(18);
    CameraSystem.OnUpdate(1.0F / 60.0F);
    Result = Invoke(CameraSystem.BuildSnapshot());
    EXPECT_EQ(Result.Output["selection"]["readback"]["status"], "pending");
    EXPECT_EQ(Result.Output["selection"]["readback"]["latestKnown"]["status"], "available");
    EXPECT_EQ(Result.Output["selection"]["readback"]["latestKnown"]["entity"], 18);

    Fake->Payload = std::vector<std::byte>(2);
    CameraSystem.OnUpdate(1.0F / 60.0F);
    Result = Invoke(CameraSystem.BuildSnapshot());
    EXPECT_EQ(Result.Output["selection"]["readback"]["status"], "unavailable");
    EXPECT_EQ(Result.Output["selection"]["readback"]["reason"], "malformed_payload");
    EXPECT_TRUE(Result.Output["selection"]["readback"]["isPending"]);
    EXPECT_EQ(Result.Output["selection"]["readback"]["latestKnown"]["status"], "available");
    EXPECT_EQ(Result.Output["selection"]["readback"]["latestKnown"]["entity"], 18);

    Fake->Payload.clear();
    CameraSystem.OnUpdate(1.0F / 60.0F);
    Result = Invoke(CameraSystem.BuildSnapshot());
    EXPECT_EQ(Result.Output["selection"]["readback"]["status"], "pending");
    EXPECT_EQ(Result.Output["selection"]["readback"]["reason"], "scheduled_readback_incomplete");
    EXPECT_TRUE(Result.Output["selection"]["readback"]["isPending"]);
    EXPECT_EQ(Result.Output["selection"]["readback"]["latestKnown"]["status"], "available");
    EXPECT_EQ(Result.Output["selection"]["readback"]["latestKnown"]["entity"], 18);

    Camera.IsReadbackScheduled = false;
    Fake->Payload              = EntityPayload(GBuffer::BackgroundEntityId);
    CameraSystem.OnUpdate(1.0F / 60.0F);
    Result = Invoke(CameraSystem.BuildSnapshot());
    EXPECT_EQ(Result.Output["selection"]["readback"]["status"], "latest_known");
    EXPECT_TRUE(Result.Output["selection"]["readback"]["latestKnown"]["entity"].is_null());

    Camera.IsReadbackScheduled = false;
    Fake->Payload              = std::vector<std::byte>(2);
    CameraSystem.OnUpdate(1.0F / 60.0F);
    Result = Invoke(CameraSystem.BuildSnapshot());
    EXPECT_EQ(Result.Output["selection"]["readback"]["status"], "unavailable");
    EXPECT_EQ(Result.Output["selection"]["readback"]["reason"], "malformed_payload");
    EXPECT_EQ(Result.Output["selection"]["readback"]["latestKnown"]["status"], "available");
    EXPECT_TRUE(Result.Output["selection"]["readback"]["latestKnown"]["entity"].is_null());

    Camera.IsReadbackScheduled = false;
    Fake->Payload              = EntityPayload(23);
    CameraSystem.OnUpdate(1.0F / 60.0F);
    Result = Invoke(CameraSystem.BuildSnapshot());
    EXPECT_EQ(Result.Output["selection"]["readback"]["latestKnown"]["entity"], 23);
    EXPECT_EQ(Fake->Reads, 9);

    Scene      SceneValue;
    const auto StaleEntity = static_cast<entt::entity>(999);
    EditorValue.BindScene(SceneValue);
    Camera.SelectedEntity      = StaleEntity;
    Camera.IsReadbackScheduled = false;
    Fake->Payload              = EntityPayload(entt::to_integral(StaleEntity));
    CameraSystem.OnUpdate(1.0F / 60.0F);
    Result = Invoke(CameraSystem.BuildSnapshot());
    ASSERT_EQ(Result.Status, CommandStatus::Succeeded);
    EXPECT_EQ(Result.Output["selection"]["status"], "unavailable");
    EXPECT_EQ(Result.Output["selection"]["reason"], "selected_entity_not_in_scene");
    EXPECT_TRUE(Result.Output["selection"]["entity"].is_null());
    EXPECT_EQ(Result.Output["selection"]["readback"]["latestKnown"]["status"], "available");
    EXPECT_EQ(Result.Output["selection"]["readback"]["latestKnown"]["entity"], entt::to_integral(StaleEntity));
    EXPECT_EQ(Result.Output["selection"]["readback"]["latestKnown"]["isValidInScene"], false);
    EditorValue.UnbindScene();
    EXPECT_EQ(Fake->Reads, 10);

    Camera.Readback = nullptr;
    CameraSystem.OnUpdate(1.0F / 60.0F);
    Result = Invoke(CameraSystem.BuildSnapshot());
    EXPECT_EQ(Result.Output["selection"]["readback"]["status"], "unavailable");
    EXPECT_EQ(Result.Output["selection"]["readback"]["reason"], "readback_unavailable");
    EXPECT_EQ(Result.Output["selection"]["readback"]["latestKnown"]["status"], "unavailable");
    EXPECT_TRUE(Result.Output["selection"]["readback"]["latestKnown"]["entity"].is_null());
    EXPECT_EQ(Fake->Reads, 10);

    EditorValue.UnbindCommands();
    EditorValue.Shutdown();
}
