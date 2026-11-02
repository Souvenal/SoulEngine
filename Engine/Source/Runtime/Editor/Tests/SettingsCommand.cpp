#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
import std;
import Core;
import EditorCommands;
import Editor.UIManager;
using namespace SoulEngine;
TEST(SettingsPanelCommandIntegration, UserAndAgentToggleActualSettingsThroughOneQueuedHandler) {
    CommandDispatcher Commands;
    auto&             UI = UIManager::Get();
    UI.Register("Settings", []() -> void {});
    auto Token = UI.RegisterSettingsCommand(Commands);
    ASSERT_TRUE(Token);
    ASSERT_TRUE(Commands.SubmitByUser("editor.panel.toggle", String("Settings")));
    EXPECT_FALSE(UI.IsUIShowing("Settings"));
    Commands.ExecutePhase(CommandPhase::Action, 1);
    EXPECT_TRUE(UI.IsUIShowing("Settings"));
    ASSERT_TRUE(Commands.SubmitByAgent("editor.panel.toggle", nlohmann::json{{"panel", "Settings"}}));
    Commands.ExecutePhase(CommandPhase::Action, 2);
    EXPECT_FALSE(UI.IsUIShowing("Settings"));
    Commands.Retire(*Token);
}
