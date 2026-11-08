module;

#include <imgui.h>

module Editor.SettingsPanel;

import Core;
import Editor.UIManager;
import std;

namespace SoulEngine {

/// @brief One editable setting: the dotted name plus the registry entry
///        backing it.  Exactly one of the two pointers is set; values are
///        read through the entry every frame, so the panel never shows a
///        stale snapshot.
struct SettingView {
    StringView               Name;
    const CVarEntry<bool>*   Bool = nullptr;
    const CVarEntry<String>* Str  = nullptr;
};

[[nodiscard]] auto GetSettingLeaf(StringView Name) -> String {
    const auto LeafOffset = Name.find_last_of('.');
    return String{Name.substr(LeafOffset == StringView::npos ? 0 : LeafOffset + 1)};
}

/// @brief Gather bool CVars and String CVars that declare options (including enum CVars, whose entries
///        carry the reflected enumerator names), sorted by dotted name.  Registration is
///        static-init only, so the entry references stay valid across frames.
[[nodiscard]] auto CollectSettings() -> std::vector<SettingView> {
    std::vector<SettingView> Result = {};
    CVarManager::Get().ForEach<bool>([&](const StringView Name, const CVarEntry<bool>& Entry) -> void {
        Result.push_back({.Name = Name, .Bool = &Entry, .Str = nullptr});
    });
    CVarManager::Get().ForEach<String>([&](const StringView Name, const CVarEntry<String>& Entry) -> void {
        if (!Entry.Options.empty())
            Result.push_back({.Name = Name, .Bool = nullptr, .Str = &Entry});
    });
    std::ranges::sort(Result, {}, &SettingView::Name);
    return Result;
}

template <CVarStorage T>
auto DrawSettingMetadata(StringView Name, const CVarEntry<T>& Entry) -> void {
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(String{Name}.c_str());
        ImGui::TextUnformatted(Entry.Description.c_str());
        ImGui::EndTooltip();
    }
    if (Entry.Timing == CVarTiming::Startup) {
        ImGui::SameLine();
        ImGui::TextUnformatted("(requires restart)");
    }
}

auto DrawBooleanSetting(const SettingView& Setting) -> void {
    const auto Leaf  = GetSettingLeaf(Setting.Name);
    auto       Value = CVarManager::Get().LoadEntry(*Setting.Bool);
    ImGui::PushID(static_cast<const void*>(Setting.Bool));
    if (ImGui::Checkbox(Leaf.c_str(), &Value))
        CVarManager::Get().SetByName<bool>(Setting.Name, Value);
    DrawSettingMetadata(Setting.Name, *Setting.Bool);
    ImGui::PopID();
}

auto DrawOptionSetting(const SettingView& Setting) -> void {
    const auto Leaf    = GetSettingLeaf(Setting.Name);
    const auto Current = CVarManager::Get().LoadEntry(*Setting.Str);
    ImGui::PushID(static_cast<const void*>(Setting.Str));
    if (ImGui::BeginCombo(Leaf.c_str(), Current.c_str())) {
        for (const auto& Option : Setting.Str->Options) {
            const bool IsSelected = Option == Current;
            if (ImGui::Selectable(Option.c_str(), IsSelected))
                CVarManager::Get().SetByName<String>(Setting.Name, Option);
            if (IsSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    DrawSettingMetadata(Setting.Name, *Setting.Str);
    ImGui::PopID();
}

auto DrawSetting(const SettingView& Setting) -> void {
    if (Setting.Bool != nullptr)
        DrawBooleanSetting(Setting);
    else
        DrawOptionSetting(Setting);
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
