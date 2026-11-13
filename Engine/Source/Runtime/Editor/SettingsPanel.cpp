module;

#include <imgui.h>

module Editor.SettingsPanel;

import Core;
import Editor.UIManager;
import std;

namespace SoulEngine {

/// @brief One editable setting: the dotted name plus its type-erased
///        registry entry.  Values are read through the entry every frame,
///        so the panel never shows a stale snapshot.
struct SettingView {
    StringView Name;
    EntryBase* Entry = nullptr;
};

[[nodiscard]] auto GetSettingLeaf(StringView Name) -> String {
    const auto LeafOffset = Name.find_last_of('.');
    return String{Name.substr(LeafOffset == StringView::npos ? 0 : LeafOffset + 1)};
}

/// @brief Gather bool CVars and option-bearing CVars (including enums, whose entries carry every
///        reflected enumerator), sorted by dotted name.  Registration is static-init only, so the entry
///        references stay valid across frames.
[[nodiscard]] auto CollectSettings() -> std::vector<SettingView> {
    std::vector<SettingView> Result = {};
    CVarManager::Get().ForEachEntry([&](const StringView Name, EntryBase& Entry) -> void {
        if (Entry.TypeId() == CVarTypeId<bool>() || !Entry.OptionNames().empty())
            Result.push_back({.Name = Name, .Entry = &Entry});
    });
    std::ranges::sort(Result, {}, &SettingView::Name);
    return Result;
}

/// @brief Shared metadata tooltip for any setting.
auto DrawSettingMetadata(StringView Name, const EntryBase& Entry) -> void {
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(String{Name}.c_str());
        ImGui::TextUnformatted(String{Entry.Description()}.c_str());
        ImGui::EndTooltip();
    }
    if (Entry.Timing() == CVarTiming::Startup) {
        ImGui::SameLine();
        ImGui::TextUnformatted("(requires restart)");
    }
}

auto DrawBooleanSetting(StringView Name, const EntryBase& Entry) -> void {
    const auto Leaf  = GetSettingLeaf(Name);
    auto       Value = CVarManager::Get().Find<bool>(Name).value_or(false);
    if (ImGui::Checkbox(Leaf.c_str(), &Value))
        CVarManager::Get().SetByName<bool>(Name, Value);
}

auto DrawOptionSetting(StringView Name, EntryBase& Entry) -> void {
    const auto Leaf    = GetSettingLeaf(Name);
    const auto Current = Entry.LoadAsString();
    if (ImGui::BeginCombo(Leaf.c_str(), Current.c_str())) {
        for (const auto& Option : Entry.OptionNames()) {
            const bool IsSelected = Option == Current;
            if (ImGui::Selectable(Option.c_str(), IsSelected))
                static_cast<void>(Entry.ApplyFromString(Option));
            if (IsSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

auto DrawSetting(SettingView& Setting) -> void {
    auto& Entry = *Setting.Entry;
    if (Entry.TypeId() == CVarTypeId<bool>())
        DrawBooleanSetting(Setting.Name, Entry);
    else
        DrawOptionSetting(Setting.Name, Entry);
    DrawSettingMetadata(Setting.Name, Entry);
}

auto RegisterSettingsPanel() -> void {
    UIManager::Get().Register("Settings", [SelectedCategory = String{}]() mutable -> void {
        bool IsShowing = UIManager::Get().IsUIShowing("Settings");
        ImGui::SetNextWindowSize(ImVec2(680.0F, 460.0F), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Settings", &IsShowing)) {
            auto             Settings   = CollectSettings();
            std::set<String> Categories = {};
            for (auto& Setting : Settings) {
                Categories.emplace(String{Setting.Name.substr(0, Setting.Name.find('.'))});
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
                    std::map<String, std::vector<SettingView*>> Groups = {};
                    for (auto& Setting : Settings) {
                        const auto FirstDot = Setting.Name.find('.');
                        if (Setting.Name.substr(0, FirstDot) != SelectedCategory)
                            continue;
                        const auto SecondDot =
                            FirstDot == StringView::npos ? StringView::npos : Setting.Name.find('.', FirstDot + 1);
                        if (SecondDot == StringView::npos) {
                            DrawSetting(Setting);
                        } else {
                            Groups[String{Setting.Name.substr(FirstDot + 1, SecondDot - FirstDot - 1)}].push_back(
                                &Setting);
                        }
                    }
                    for (const auto& [Group, Entries] : Groups) {
                        if (ImGui::CollapsingHeader(Group.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                            for (auto* Setting : Entries)
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
