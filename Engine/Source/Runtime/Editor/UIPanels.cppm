module;
#include <imgui.h>

export module Editor:UIPanels;

import std;
import Core;
import Resource;
import :UIManager;

export namespace SoulEngine {

// ============ 注册所有 UI ============
auto RegisterAllUI() -> void {
    // Materials Window
    UIManager::Get().Register(
        "Debug.ShowMaterials",
        []() {
            auto& entry = UIManager::Get().GetAll().at("Debug.ShowMaterials");
            if (ImGui::Begin("Materials", &entry.Show)) {
                auto materials = MaterialManager::Get().GetMaterials();

                if (materials.empty()) {
                    ImGui::Text("No materials loaded.");
                } else {
                    ImGui::Text("Total materials: %zu", materials.size());
                    ImGui::Separator();

                    // 表格显示
                    if (ImGui::BeginTable("MaterialsTable",
                                          3,
                                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                              ImGuiTableFlags_Resizable)) {

                        ImGui::TableSetupColumn("ID");
                        ImGui::TableSetupColumn("Name");
                        ImGui::TableSetupColumn("Base Color");
                        ImGui::TableHeadersRow();

                        for (const auto& mat : materials) {
                            ImGui::TableNextRow();

                            ImGui::TableSetColumnIndex(0);
                            ImGui::Text("%u", mat.Id);

                            ImGui::TableSetColumnIndex(1);
                            ImGui::Text("%s", mat.Name.c_str());

                            ImGui::TableSetColumnIndex(2);
                            ImGui::ColorEdit4(("##Color" + std::to_string(mat.Id)).c_str(),
                                              const_cast<float*>(reinterpret_cast<const float*>(&mat.Value.BaseColor)),
                                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
                        }

                        ImGui::EndTable();
                    }
                }
            }
            ImGui::End();
        },
        false);

    // 未来在这里添加更多 UI
    // UIManager::Get().Register("xxx", []() { ... }, false);
}

} // namespace SoulEngine
