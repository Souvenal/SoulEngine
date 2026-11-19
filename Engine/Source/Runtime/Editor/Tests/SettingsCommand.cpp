#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "HeadlessEditor.h"

import std;
import Core;
import Editor.SettingsPanel;
import Editor.UIManager;
import EditorCommands;

namespace SoulEngine {

CVar<bool> CVarEditorCommandBoolean{"AEditorCommands.Boolean", false, "Editor command test Boolean"};
CVar<bool> CVarEditorCommandStartupBoolean{
    "AEditorCommands.StartupBoolean", false, "Editor command test startup Boolean", CVarTiming::Startup};
CVar<Int32> CVarEditorCommandWrongType{"AEditorCommands.WrongType", 0, "Editor command test non-Boolean"};

TEST(PanelCommandIntegration, UserAndAgentShareToggleCloseAndExecutionValidation) {
    CommandDispatcher Commands;
    auto&             UI = UIManager::Get();
    UI.Register("Settings", []() -> void {});
    UI.Register("Debug.ShowMaterials", []() -> void {});
    auto Tokens = UI.RegisterPanelCommands(Commands);
    ASSERT_TRUE(Tokens) << Tokens.error().ToString();

    ASSERT_TRUE(UI.SubmitPanelToggle(Commands, "Settings"));
    EXPECT_FALSE(UI.IsUIShowing("Settings"));
    Commands.ExecutePhase(CommandPhase::Action, 1);
    EXPECT_TRUE(UI.IsUIShowing("Settings"));

    std::optional<CommandResult> AgentResult;
    ASSERT_TRUE(Commands.SubmitByAgent("editor.panel.toggle",
                                       nlohmann::json{{"panel", "Settings"}},
                                       [&](CommandResult Result) -> void { AgentResult = std::move(Result); }));
    Commands.ExecutePhase(CommandPhase::Action, 2);
    ASSERT_TRUE(AgentResult);
    EXPECT_EQ(AgentResult->Status, CommandStatus::Succeeded);
    EXPECT_FALSE(UI.IsUIShowing("Settings"));

    ASSERT_TRUE(Commands.SubmitByAgent("editor.panel.toggle", nlohmann::json{{"panel", "Debug.ShowMaterials"}}));
    Commands.ExecutePhase(CommandPhase::Action, 3);
    EXPECT_TRUE(UI.IsUIShowing("Debug.ShowMaterials"));
    ASSERT_TRUE(UI.SubmitPanelClose(Commands, "Debug.ShowMaterials"));
    Commands.ExecutePhase(CommandPhase::Action, 4);
    EXPECT_FALSE(UI.IsUIShowing("Debug.ShowMaterials"));
    ASSERT_TRUE(UI.SubmitPanelClose(Commands, "Debug.ShowMaterials"));
    Commands.ExecutePhase(CommandPhase::Action, 5);
    EXPECT_FALSE(UI.IsUIShowing("Debug.ShowMaterials"));
    EXPECT_TRUE(Commands.TakeDiagnostics().empty());

    AgentResult.reset();
    ASSERT_TRUE(Commands.SubmitByAgent("editor.panel.toggle",
                                       nlohmann::json{{"panel", "Missing.Panel"}},
                                       [&](CommandResult Result) -> void { AgentResult = std::move(Result); }));
    Commands.ExecutePhase(CommandPhase::Action, 6);
    ASSERT_TRUE(AgentResult);
    ASSERT_TRUE(AgentResult->Failure);
    EXPECT_EQ(AgentResult->Failure->Code, "stale_target");
    EXPECT_EQ(AgentResult->Status, CommandStatus::Failed);

    AgentResult.reset();
    EXPECT_FALSE(Commands.SubmitByAgent("editor.panel.close",
                                        nlohmann::json{{"panel", "Settings"}},
                                        [&](CommandResult Result) -> void { AgentResult = std::move(Result); }));
    ASSERT_TRUE(AgentResult);
    ASSERT_TRUE(AgentResult->Failure);
    EXPECT_EQ(AgentResult->Failure->Code, "denied_exposure");

    Commands.Retire(Tokens->Toggle);
    Commands.Retire(Tokens->Close);
}

TEST(SettingsCommandIntegration, OwningNamedBooleanCallsPreserveValuesAndFailureDiagnostics) {
    CommandDispatcher Commands;
    auto              Token = RegisterSettingsCommand(Commands);
    ASSERT_TRUE(Token) << Token.error().ToString();
    auto& Manager = CVarManager::Get();
    ASSERT_TRUE(Manager.SetByName<bool>(CVarEditorCommandBoolean.Name(), false));
    ASSERT_TRUE(Manager.SetByName<bool>(CVarEditorCommandStartupBoolean.Name(), false));

    ASSERT_TRUE(Commands.SubmitByUser("editor.settings.set_boolean", CVarEditorCommandBoolean.Name(), true));
    EXPECT_EQ(Manager.Find<bool>(CVarEditorCommandBoolean.Name()), false);
    Commands.ExecutePhase(CommandPhase::Action, 1);
    EXPECT_EQ(Manager.Find<bool>(CVarEditorCommandBoolean.Name()), true);

    std::vector<CommandResult> Results;
    const auto                 Completion = [&](CommandResult Result) -> void { Results.push_back(std::move(Result)); };
    ASSERT_TRUE(Commands.SubmitByAgent("editor.settings.set_boolean",
                                       nlohmann::json{{"value", false}, {"name", CVarEditorCommandBoolean.Name()}},
                                       Completion));
    Commands.ExecutePhase(CommandPhase::Action, 2);
    EXPECT_EQ(Manager.Find<bool>(CVarEditorCommandBoolean.Name()), false);

    ASSERT_TRUE(Commands.SubmitByAgent("editor.settings.set_boolean",
                                       nlohmann::json{{"name", CVarEditorCommandBoolean.Name()}, {"value", false}},
                                       Completion));
    Commands.ExecutePhase(CommandPhase::Action, 3);
    EXPECT_EQ(Manager.Find<bool>(CVarEditorCommandBoolean.Name()), false);

    ASSERT_TRUE(
        Commands.SubmitByAgent("editor.settings.set_boolean",
                               nlohmann::json{{"name", CVarEditorCommandStartupBoolean.Name()}, {"value", true}},
                               Completion));
    Commands.ExecutePhase(CommandPhase::Action, 4);
    const auto StartupRecords = Manager.Collect<bool>();
    const auto Startup =
        std::ranges::find(StartupRecords, CVarEditorCommandStartupBoolean.Name(), &CVarSnapshot<bool>::Name);
    ASSERT_NE(Startup, StartupRecords.end());
    EXPECT_TRUE(Startup->Value);
    EXPECT_EQ(Startup->Timing, CVarTiming::Startup);

    ASSERT_TRUE(Commands.SubmitByAgent("editor.settings.set_boolean",
                                       nlohmann::json{{"name", "AEditorCommands.Missing"}, {"value", true}},
                                       Completion));
    Commands.ExecutePhase(CommandPhase::Action, 5);
    ASSERT_TRUE(Commands.SubmitByAgent("editor.settings.set_boolean",
                                       nlohmann::json{{"name", CVarEditorCommandWrongType.Name()}, {"value", true}},
                                       Completion));
    Commands.ExecutePhase(CommandPhase::Action, 6);

    ASSERT_EQ(Results.size(), 5);
    EXPECT_EQ(Results[0].Status, CommandStatus::Succeeded);
    EXPECT_EQ(Results[1].Status, CommandStatus::Succeeded);
    EXPECT_EQ(Results[2].Status, CommandStatus::Succeeded);
    ASSERT_TRUE(Results[3].Failure);
    EXPECT_EQ(Results[3].Failure->Code, "stale_target");
    ASSERT_TRUE(Results[4].Failure);
    EXPECT_EQ(Results[4].Failure->Code, "stale_target");
    EXPECT_EQ(Manager.Find<bool>(CVarEditorCommandBoolean.Name()), false);
    EXPECT_EQ(Manager.Find<Int32>(CVarEditorCommandWrongType.Name()), 0);

    std::optional<CommandResult> InvalidArguments;
    EXPECT_FALSE(Commands.SubmitByAgent("editor.settings.set_boolean",
                                        nlohmann::json{{"name", CVarEditorCommandBoolean.Name()}, {"value", "true"}},
                                        [&](CommandResult Result) -> void { InvalidArguments = std::move(Result); }));
    ASSERT_TRUE(InvalidArguments);
    ASSERT_TRUE(InvalidArguments->Failure);
    EXPECT_EQ(InvalidArguments->Failure->Code, "invalid_arguments");
    Commands.Retire(*Token);
}

TEST(SettingsCommandIntegration, ProductionMenuCheckboxAndCloseUseActionPhase) {
    auto& Manager = CVarManager::Get();
    ASSERT_TRUE(Manager.SetByName<bool>(CVarEditorCommandBoolean.Name(), false));
    HeadlessEditor Fixture;
    ASSERT_TRUE(Fixture.Bind());
    Fixture.Frame();

    const float SettingsX = MainMenuX({"Settings"}, "Settings");
    Fixture.Click(ImVec2(SettingsX, 10.0F));
    Fixture.Click(ImVec2(SettingsX, 32.0F));
    EXPECT_FALSE(UIManager::Get().IsUIShowing("Settings"));
    Fixture.DispatchAction();
    EXPECT_TRUE(UIManager::Get().IsUIShowing("Settings"));

    Fixture.Frame();
    const auto* Options = FindImGuiWindow("Settings/Options");
    ASSERT_NE(Options, nullptr);
    const float CheckboxOffset = ImGui::GetFrameHeight() * 0.5F;
    Fixture.Click(ImVec2(Options->Pos.x + Options->WindowPadding.x + CheckboxOffset,
                         Options->Pos.y + Options->WindowPadding.y + CheckboxOffset));
    EXPECT_EQ(Manager.Find<bool>(CVarEditorCommandBoolean.Name()), false);
    Fixture.DispatchAction();
    EXPECT_EQ(Manager.Find<bool>(CVarEditorCommandBoolean.Name()), true);

    Fixture.Frame();
    ASSERT_TRUE(ClickImGuiWindowClose(Fixture, "Settings"));
    EXPECT_TRUE(UIManager::Get().IsUIShowing("Settings"));
    Fixture.DispatchAction();
    EXPECT_FALSE(UIManager::Get().IsUIShowing("Settings"));

    ASSERT_TRUE(Fixture.Commands.SubmitByAgent("editor.panel.toggle", nlohmann::json{{"panel", "Settings"}}));
    Fixture.DispatchAction();
    EXPECT_TRUE(UIManager::Get().IsUIShowing("Settings"));
    Fixture.Frame();
    EXPECT_NE(FindImGuiWindow("Settings"), nullptr);
}

TEST(PanelCommandIntegration, ProductionMaterialsMenuAndCloseUseActionPhase) {
    HeadlessEditor Fixture;
    ASSERT_TRUE(Fixture.Bind());
    Fixture.Frame();

    const float DebugX = MainMenuX({"Settings", "Project", "Render", "Debug"}, "Debug");
    Fixture.Click(ImVec2(DebugX, 10.0F));
    Fixture.Click(ImVec2(DebugX, 32.0F));
    EXPECT_FALSE(UIManager::Get().IsUIShowing("Debug.ShowMaterials"));
    Fixture.DispatchAction();
    EXPECT_TRUE(UIManager::Get().IsUIShowing("Debug.ShowMaterials"));

    Fixture.Frame();
    ASSERT_TRUE(ClickImGuiWindowClose(Fixture, "Materials"));
    EXPECT_TRUE(UIManager::Get().IsUIShowing("Debug.ShowMaterials"));
    Fixture.DispatchAction();
    EXPECT_FALSE(UIManager::Get().IsUIShowing("Debug.ShowMaterials"));

    ASSERT_TRUE(
        Fixture.Commands.SubmitByAgent("editor.panel.toggle", nlohmann::json{{"panel", "Debug.ShowMaterials"}}));
    Fixture.DispatchAction();
    EXPECT_TRUE(UIManager::Get().IsUIShowing("Debug.ShowMaterials"));
    ASSERT_TRUE(UIManager::Get().SubmitPanelClose(Fixture.Commands, "Debug.ShowMaterials"));
    ASSERT_TRUE(UIManager::Get().SubmitPanelClose(Fixture.Commands, "Debug.ShowMaterials"));
    Fixture.DispatchAction();
    EXPECT_FALSE(UIManager::Get().IsUIShowing("Debug.ShowMaterials"));
}

} // namespace SoulEngine
