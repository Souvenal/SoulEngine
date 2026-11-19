module;

#include <imgui.h>

module Editor.SettingsPanel;

import Core;
import EditorCommands;
import Editor.UIManager;
import std;

namespace SoulEngine {

auto DrawBooleanSetting(const CVarSnapshot<bool>& Setting, CommandDispatcher& Commands) -> void {
    const auto LeafOffset = Setting.Name.find_last_of('.');
    const auto Leaf       = Setting.Name.substr(LeafOffset == String::npos ? 0 : LeafOffset + 1);
    auto       Value      = Setting.Value;
    ImGui::PushID(Setting.Name.c_str());
    if (ImGui::Checkbox(Leaf.c_str(), &Value)) {
        if (auto Submitted = Commands.SubmitByUser("editor.settings.set_boolean", Setting.Name, Value); !Submitted)
            LogError("Boolean setting command failed: {}", Submitted.error().ToString());
    }
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

auto RegisterSettingsCommand(CommandDispatcher& Commands) -> std::expected<Uint64, ErrorMessage> {
    return Commands.Register(CommandDescriptor{.Name           = "editor.settings.set_boolean",
                                               .Parameters     = {{.Name = "name"}, {.Name = "value"}},
                                               .IsAgentExposed = true,
                                               .Category       = CommandCategory::Input},
                             [](CommandContext& Context, String Name, bool Value) -> void {
                                 auto&      Manager = CVarManager::Get();
                                 const auto Current = Manager.Find<bool>(Name);
                                 if (!Current) {
                                     Context.Fail("stale_target",
                                                  Format("Boolean setting '{}' is unavailable", Name),
                                                  CommandJson{{"name", Name}});
                                     return;
                                 }
                                 if (*Current == Value)
                                     return;
                                 if (!Manager.SetByName<bool>(Name, Value))
                                     Context.Fail("stale_target",
                                                  Format("Boolean setting '{}' is unavailable", Name),
                                                  CommandJson{{"name", Name}});
                             });
}

auto RegisterSettingsPanel(CommandDispatcher& Commands) -> void {
    UIManager::Get().Register("Settings", [SelectedCategory = String{}, Commands = &Commands]() mutable -> void {
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
                            DrawBooleanSetting(Setting, *Commands);
                        } else {
                            Groups[Setting.Name.substr(FirstDot + 1, SecondDot - FirstDot - 1)].push_back(&Setting);
                        }
                    }
                    for (const auto& [Group, Entries] : Groups) {
                        if (ImGui::CollapsingHeader(Group.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                            for (const auto* Setting : Entries)
                                DrawBooleanSetting(*Setting, *Commands);
                        }
                    }
                }
                ImGui::EndChild();
            }
        }
        ImGui::End();
        if (!IsShowing) {
            if (auto Submitted = UIManager::Get().SubmitPanelClose(*Commands, "Settings"); !Submitted)
                LogError("Settings close command failed: {}", Submitted.error().ToString());
        }
    });
}

} // namespace SoulEngine
