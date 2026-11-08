module;

#include <imgui.h>

module Editor.SettingsPanel;

import Core;
import Editor.UIManager;
import std;

namespace SoulEngine {

/// @brief One editable setting: the dotted name plus the registry entry
///        backing it.  Bool points at a typed entry; General points at the
///        type-erased entry of an option-bearing non-bool CVar.  Values are
///        read through the entry every frame, so the panel never shows a
///        stale snapshot.
struct SettingView {
    StringView       Name;
    CVarEntry<bool>* Bool    = nullptr;
    EntryBase*       General = nullptr;
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
    CVarManager::Get().ForEach<bool>([&](const StringView Name, CVarEntry<bool>& Entry) -> void {
        Result.push_back({.Name = Name, .Bool = &Entry, .General = nullptr});
    });
    CVarManager::Get().ForEachEntry([&](const StringView Name, EntryBase& Entry) -> void {
        if (!Entry.OptionNames().empty())
            Result.push_back({.Name = Name, .Bool = nullptr, .General = &Entry});
    });
    std::ranges::sort(Result, {}, &SettingView::Name);
    return Result;
}

/// @brief Shared metadata tooltip for any setting; bool settings draw from
///        the typed entry, option-bearing ones from the type-erased entry.
auto DrawSettingTooltip(StringView Name, StringView Description, CVarTiming Timing) -> void {
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(String{Name}.c_str());
        ImGui::TextUnformatted(String{Description}.c_str());
        ImGui::EndTooltip();
    }
    if (Timing == CVarTiming::Startup) {
        ImGui::SameLine();
        ImGui::TextUnformatted("(requires restart)");
    }
}

auto DrawSettingMetadata(StringView Name, const CVarEntry<bool>& Entry) -> void {
    DrawSettingTooltip(Name, Entry.Description, Entry.Timing);
}

auto DrawSettingMetadata(StringView Name, const EntryBase& Entry) -> void {
    DrawSettingTooltip(Name, Entry.Description(), Entry.Timing());
}

auto DrawBooleanSetting(const SettingView& Setting) -> void {
    const auto Leaf  = GetSettingLeaf(Setting.Name);
    auto       Value = CVarManager::Get().LoadEntry(*Setting.Bool);
    ImGui::PushID(static_cast<const void*>(Setting.Bool));
    if (ImGui::Checkbox(Leaf.c_str(), &Value))
        CVarManager::Get().SetByName<bool>(Setting.Name, Value);
    ImGui::PopID();
}

auto DrawOptionSetting(const SettingView& Setting) -> void {
    const auto Leaf    = GetSettingLeaf(Setting.Name);
    const auto Current = Setting.General->LoadAsString();
    ImGui::PushID(static_cast<const void*>(Setting.General));
    if (ImGui::BeginCombo(Leaf.c_str(), Current.c_str())) {
        for (const auto& Option : Setting.General->OptionNames()) {
            const bool IsSelected = Option == Current;
            if (ImGui::Selectable(Option.c_str(), IsSelected))
                static_cast<void>(Setting.General->ApplyFromString(Option));
            if (IsSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
}

auto DrawSetting(const SettingView& Setting) -> void {
    if (Setting.Bool != nullptr) {
        DrawBooleanSetting(Setting);
        DrawSettingMetadata(Setting.Name, *Setting.Bool);
    } else {
        DrawOptionSetting(Setting);
        DrawSettingMetadata(Setting.Name, *Setting.General);
    }
}

auto RegisterSettingsPanel() -> void {
    UIManager::Get().Register("Settings", [SelectedCategory = String{}]() mutable -> void {
        bool IsShowing = UIManager::Get().IsUIShowing("Settings");
        ImGui::SetNextWindowSize(ImVec2(680.0F, 460.0F), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Settings", &IsShowing)) {
            const auto       Settings   = CollectSettings();
            std::set<String> Categories = {};
            for (const auto& Setting : Settings) {
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
                    std::map<String, std::vector<const SettingView*>> Groups = {};
                    for (const auto& Setting : Settings) {
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
