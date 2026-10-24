module;

#include <imgui.h>

module Editor.SettingsPanel;

import Core;
import Editor.UIManager;
import std;

namespace SoulEngine {

auto DrawBooleanSetting(const CVarSnapshot<bool>& Setting) -> void {
    const auto LeafOffset = Setting.Name.find_last_of('.');
    const auto Leaf       = Setting.Name.substr(LeafOffset == String::npos ? 0 : LeafOffset + 1);
    auto       Value      = Setting.Value;
    ImGui::PushID(Setting.Name.c_str());
    if (ImGui::Checkbox(Leaf.c_str(), &Value))
        CVarManager::Get().SetByName<bool>(Setting.Name, Value);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(Setting.Name.c_str());
        ImGui::TextUnformatted(Setting.Description.c_str());
        ImGui::EndTooltip();
    }
    if (Setting.Timing == CVarTiming::Startup) {
        ImGui::SameLine();
        ImGui::TextUnformatted("(requires restart)");
    }
    ImGui::PopID();
}

auto RegisterSettingsPanel() -> void {
    UIManager::Get().Register("Settings", [SelectedCategory = String{}]() mutable -> void {
        bool IsShowing = UIManager::Get().IsUIShowing("Settings");
        ImGui::SetNextWindowSize(ImVec2(680.0F, 460.0F), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Settings", &IsShowing)) {
            const auto       Settings   = CVarManager::Get().Collect<bool>();
            std::set<String> Categories = {};
            for (const auto& Setting : Settings)
                Categories.emplace(Setting.Name.substr(0, Setting.Name.find('.')));
            if (!Categories.contains(SelectedCategory) && !Categories.empty())
                SelectedCategory = *Categories.begin();

            if (Settings.empty()) {
                ImGui::TextUnformatted("No boolean settings registered.");
            } else {
                const auto CategoryWidth = std::min(180.0F, ImGui::GetContentRegionAvail().x * 0.3F);
                if (ImGui::BeginChild("Categories", ImVec2(CategoryWidth, 0.0F), ImGuiChildFlags_Borders)) {
                    for (const auto& Category : Categories) {
                        if (ImGui::Selectable(Category.c_str(), SelectedCategory == Category))
                            SelectedCategory = Category;
                    }
                }
                ImGui::EndChild();
                ImGui::SameLine();
                if (ImGui::BeginChild("Options", ImVec2(0.0F, 0.0F))) {
                    std::map<String, std::vector<const CVarSnapshot<bool>*>> Groups = {};
                    for (const auto& Setting : Settings) {
                        const auto FirstDot = Setting.Name.find('.');
                        if (Setting.Name.substr(0, FirstDot) != SelectedCategory)
                            continue;
                        const auto SecondDot =
                            FirstDot == String::npos ? String::npos : Setting.Name.find('.', FirstDot + 1);
                        if (SecondDot == String::npos) {
                            DrawBooleanSetting(Setting);
                        } else {
                            Groups[Setting.Name.substr(FirstDot + 1, SecondDot - FirstDot - 1)].push_back(&Setting);
                        }
                    }
                    for (const auto& [Group, Entries] : Groups) {
                        if (ImGui::CollapsingHeader(Group.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                            for (const auto* Setting : Entries)
                                DrawBooleanSetting(*Setting);
                        }
                    }
                }
                ImGui::EndChild();
            }
        }
        ImGui::End();
        if (!IsShowing)
            UIManager::Get().CloseUI("Settings");
    });
}

} // namespace SoulEngine
