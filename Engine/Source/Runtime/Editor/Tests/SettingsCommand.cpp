#include <gtest/gtest.h>
#include <entt/entt.hpp>
#include <imgui.h>
#include <imgui_threaded_rendering.h>
#include <nlohmann/json.hpp>
import std;
import Core;
import EditorCommands;
import Editor.UIManager;
import Editor;
using namespace SoulEngine;
TEST(SettingsPanelCommandIntegration, UserAndAgentToggleActualSettingsThroughOneQueuedHandler) {
    CommandDispatcher Commands;
    auto&             UI = UIManager::Get();
    UI.Register("Settings", []() -> void {});
    auto Token = UI.RegisterSettingsCommand(Commands);
    ASSERT_TRUE(Token);
    ASSERT_TRUE(UI.SubmitSettingsToggle(Commands));
    EXPECT_FALSE(UI.IsUIShowing("Settings"));
    Commands.ExecutePhase(CommandPhase::Action, 1);
    EXPECT_TRUE(UI.IsUIShowing("Settings"));
    ASSERT_TRUE(Commands.SubmitByAgent("editor.panel.toggle", nlohmann::json{{"panel", "Settings"}}));
    Commands.ExecutePhase(CommandPhase::Action, 2);
    EXPECT_FALSE(UI.IsUIShowing("Settings"));
    Commands.Retire(*Token);
}
TEST(SettingsPanelCommandIntegration, HeadlessMouseFramesDriveProductionSettingsMenu) {
    CommandDispatcher Commands;
    Editor            EditorValue;
    EditorValue.Initialize();
    ASSERT_TRUE(EditorValue.BindCommands(Commands));
    auto& IO              = ImGui::GetIO();
    IO.IniFilename        = nullptr;
    IO.DisplaySize        = ImVec2(800, 600);
    IO.DeltaTime          = 1.0F / 60.0F;
    unsigned char* Pixels = nullptr;
    int            Width = 0, Height = 0;
    IO.Fonts->GetTexDataAsRGBA32(&Pixels, &Width, &Height);
    IO.Fonts->SetTexID(ImTextureID(1));
    IO.Fonts->TexData->SetStatus(ImTextureStatus_OK);
    auto Frame = [&](float X, float Y, bool Down) -> void {
        IO.AddMousePosEvent(X, Y);
        IO.AddMouseButtonEvent(0, Down);
        ImDrawDataSnapshot Snapshot;
        EditorValue.Tick(1.0F / 60.0F, Snapshot);
    };
    Frame(-100, -100, false);
    Frame(30, 10, false);
    Frame(30, 10, true);
    Frame(30, 10, false);
    Frame(50, 32, false);
    Frame(50, 32, true);
    Frame(50, 32, false);
    EXPECT_FALSE(UIManager::Get().IsUIShowing("Settings"));
    Commands.ExecutePhase(CommandPhase::Action, 1);
    EXPECT_TRUE(UIManager::Get().IsUIShowing("Settings"));
    ASSERT_TRUE(Commands.SubmitByAgent("editor.panel.toggle", nlohmann::json{{"panel", "Settings"}}));
    Commands.ExecutePhase(CommandPhase::Action, 2);
    EXPECT_FALSE(UIManager::Get().IsUIShowing("Settings"));
    EditorValue.UnbindCommands();
    EditorValue.Shutdown();
}
