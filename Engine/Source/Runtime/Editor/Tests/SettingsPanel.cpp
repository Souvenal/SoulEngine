#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

import Core;
import Editor.SettingsPanel;
import Editor.UIManager;
import std;

using namespace SoulEngine;

CVar<String> CVarTestSettingsOption{"AATest.Mode", "Safe", "settings option test", std::vector<String>{"Safe", "Fast"}};

[[nodiscard]] auto FindSettingsOptionsWindow() -> ImGuiWindow* {
    for (auto* Window : ImGui::GetCurrentContext()->Windows) {
        if (StringView{Window->Name}.starts_with("Settings/Options_"))
            return Window;
    }
    return nullptr;
}

[[nodiscard]] auto GetComboPosition(const ImGuiWindow& OptionsWindow) -> ImVec2 {
    return ImVec2{OptionsWindow.DC.CursorStartPos.x + 4.0F,
                  OptionsWindow.DC.CursorPosPrevLine.y + ImGui::GetFrameHeight() * 0.5F};
}

[[nodiscard]] auto FindOpenComboPopup() -> ImGuiWindow* {
    auto& OpenPopups = ImGui::GetCurrentContext()->OpenPopupStack;
    return OpenPopups.empty() ? nullptr : OpenPopups.back().Window;
}

class SettingsPanelTest : public testing::Test {
  protected:
    auto SetUp() -> void override {
        CVarTestSettingsOption.Set("Safe");
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto& IO       = ImGui::GetIO();
        IO.DisplaySize = ImVec2(1280.0F, 720.0F);
        IO.DeltaTime   = 1.0F / 60.0F;
        IO.IniFilename = nullptr;
        IO.Fonts->Build();
        IO.AddMousePosEvent(-std::numeric_limits<Float32>::max(), -std::numeric_limits<Float32>::max());

        RegisterSettingsPanel();
        UIManager::Get().OpenUI("Settings");
    }

    auto TearDown() -> void override {
        UIManager::Get().CloseUI("Settings");
        CVarTestSettingsOption.Set("Safe");
        ImGui::DestroyContext();
    }

    auto RunFrame() -> void {
        ImGui::NewFrame();
        auto& Entry = UIManager::Get().GetAll().at("Settings");
        if (Entry.Show)
            Entry.Callback();
        ImGui::Render();
    }

    auto Click(ImVec2 Position) -> void {
        auto& IO = ImGui::GetIO();
        IO.AddMousePosEvent(Position.x, Position.y);
        IO.AddMouseButtonEvent(0, true);
        RunFrame();
        IO.AddMouseButtonEvent(0, false);
        RunFrame();
    }
};

TEST_F(SettingsPanelTest, SelectsAnOptionThroughTheGenericCombo) {
    RunFrame();
    RunFrame();
    ASSERT_TRUE(ImGui::GetDrawData()->Valid);
    const auto* OptionsWindow = FindSettingsOptionsWindow();
    ASSERT_NE(OptionsWindow, nullptr);
    Click(GetComboPosition(*OptionsWindow));
    RunFrame();
    ASSERT_TRUE(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
    EXPECT_EQ(CVarTestSettingsOption.Get(), "Safe");

    auto* Popup = FindOpenComboPopup();
    ASSERT_NE(Popup, nullptr);
    ImGui::ActivateItemByID(Popup->GetID("Fast"));
    RunFrame();

    EXPECT_EQ(CVarTestSettingsOption.Get(), "Fast");
    EXPECT_EQ(CVarManager::Get().Find<String>(CVarTestSettingsOption.Name()), std::optional{String{"Fast"}});
}
