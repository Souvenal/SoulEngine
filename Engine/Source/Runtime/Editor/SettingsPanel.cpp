module;

#include <imgui.h>

module Editor.SettingsPanel;

import Core;
import Editor.UIManager;
import std;

namespace SoulEngine {

using SettingSnapshot = std::variant<CVarSnapshot<bool>, CVarSnapshot<String>>;

[[nodiscard]] auto GetSettingName(const SettingSnapshot& Setting) -> StringView {
    return std::visit([](const auto& Snapshot) -> StringView { return Snapshot.Name; }, Setting);
}

[[nodiscard]] auto GetSettingLeaf(StringView Name) -> String {
    const auto LeafOffset = Name.find_last_of('.');
    return String{Name.substr(LeafOffset == StringView::npos ? 0 : LeafOffset + 1)};
}

[[nodiscard]] auto CollectSettings() -> std::vector<SettingSnapshot> {
    std::vector<SettingSnapshot> Result = {};
    for (auto& Setting : CVarManager::Get().Collect<bool>())
        Result.emplace_back(std::move(Setting));
    for (auto& Setting : CVarManager::Get().Collect<String>()) {
        if (!Setting.Options.empty())
            Result.emplace_back(std::move(Setting));
    }
    std::ranges::sort(Result, [](const SettingSnapshot& Left, const SettingSnapshot& Right) -> bool {
        return GetSettingName(Left) < GetSettingName(Right);
    });
    return Result;
}

template <CVarValue T>
auto DrawSettingMetadata(const CVarSnapshot<T>& Setting) -> void {
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
}

auto DrawBooleanSetting(const CVarSnapshot<bool>& Setting) -> void {
    const auto Leaf  = GetSettingLeaf(Setting.Name);
    auto       Value = Setting.Value;
    ImGui::PushID(Setting.Name.c_str());
    if (ImGui::Checkbox(Leaf.c_str(), &Value))
        CVarManager::Get().SetByName<bool>(Setting.Name, Value);
    DrawSettingMetadata(Setting);
    ImGui::PopID();
}

auto DrawOptionSetting(const CVarSnapshot<String>& Setting) -> void {
    const auto Leaf = GetSettingLeaf(Setting.Name);
    ImGui::PushID(Setting.Name.c_str());
    if (ImGui::BeginCombo(Leaf.c_str(), Setting.Value.c_str())) {
        for (const auto& Option : Setting.Options) {
            const bool IsSelected = Option == Setting.Value;
            if (ImGui::Selectable(Option.c_str(), IsSelected))
                CVarManager::Get().SetByName<String>(Setting.Name, Option);
            if (IsSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    DrawSettingMetadata(Setting);
    ImGui::PopID();
}

auto DrawSetting(const SettingSnapshot& Setting) -> void {
    std::visit(
        [](const auto& Snapshot) -> void {
            using SnapshotType = std::remove_cvref_t<decltype(Snapshot)>;
            if constexpr (std::same_as<SnapshotType, CVarSnapshot<bool>>)
                DrawBooleanSetting(Snapshot);
            else
                DrawOptionSetting(Snapshot);
        },
        Setting);
}

auto RegisterSettingsPanel() -> void {
    UIManager::Get().Register("Settings", [SelectedCategory = String{}]() mutable -> void {
        bool IsShowing = UIManager::Get().IsUIShowing("Settings");
        ImGui::SetNextWindowSize(ImVec2(680.0F, 460.0F), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Settings", &IsShowing)) {
            const auto       Settings   = CollectSettings();
            std::set<String> Categories = {};
            for (const auto& Setting : Settings) {
                const auto Name = GetSettingName(Setting);
                Categories.emplace(String{Name.substr(0, Name.find('.'))});
            }
            if (!Categories.contains(SelectedCategory) && !Categories.empty())
                SelectedCategory = *Categories.begin();

            if (Settings.empty()) {
                ImGui::TextUnformatted("No editable settings registered.");
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
                    std::map<String, std::vector<const SettingSnapshot*>> Groups = {};
                    for (const auto& Setting : Settings) {
                        const auto Name     = GetSettingName(Setting);
                        const auto FirstDot = Name.find('.');
                        if (Name.substr(0, FirstDot) != SelectedCategory)
                            continue;
                        const auto SecondDot =
                            FirstDot == StringView::npos ? StringView::npos : Name.find('.', FirstDot + 1);
                        if (SecondDot == StringView::npos) {
                            DrawSetting(Setting);
                        } else {
                            Groups[String{Name.substr(FirstDot + 1, SecondDot - FirstDot - 1)}].push_back(&Setting);
                        }
                    }
                    for (const auto& [Group, Entries] : Groups) {
                        if (ImGui::CollapsingHeader(Group.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                            for (const auto* Setting : Entries)
                                DrawSetting(*Setting);
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
