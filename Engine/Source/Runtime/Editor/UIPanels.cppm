module;
#include <imgui.h>

export module Editor:UIPanels;

import std;
import Core;
import Material;
import :UIManager;

export namespace SoulEngine {

/// @brief Point the debug panels at the active scene's material store.
// ============ 注册所有 UI ============
auto RegisterAllUI() -> void {
    // Materials Window
    UIManager::Get().Register(
        "Debug.ShowMaterials",
        []() {
            auto& entry = UIManager::Get().GetAll().at("Debug.ShowMaterials");
            if (ImGui::Begin("Materials", &entry.Show)) {
                ImGui::Text("Material YAML overrides are loaded from mesh components.");
            }
            ImGui::End();
        },
        false);

    // 未来在这里添加更多 UI
    // UIManager::Get().Register("xxx", []() { ... }, false);
}

} // namespace SoulEngine
