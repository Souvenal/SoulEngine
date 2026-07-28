module;
#include <entt/entt.hpp>
#include <imgui.h>

export module Editor:MainMenu;

import std;
import Application;
import Renderer;

export namespace SoulEngine {

/// @brief Draw the editor's global main menu and queue selected actions.
auto DrawMainMenu() -> void {
    if (!ImGui::BeginMainMenuBar())
        return;

    if (ImGui::BeginMenu("Project")) {
        if (ImGui::BeginMenu("Open Application")) {
            const auto* CurrentApplication = GetCurrentApplication();
            for (const auto& Name : ApplicationFactory::Get().Keys()) {
                String Label(Name);
                const bool IsCurrent = CurrentApplication && CurrentApplication->GetName() == Name;
                if (ImGui::MenuItem(Label.c_str(), nullptr, IsCurrent) && !IsCurrent) {
                    if (auto R = OpenApplication(Name); !R)
                        LogError("Application opening failed:\n{}", R.error().ToString());
                    break;
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Render")) {
        const auto CurrentRendererName = GetCurrentRendererName();
        for (const auto& Name : RendererFactory::Get().Keys()) {
            String Label(Name);
            const bool IsCurrent = CurrentRendererName == Name;
            if (ImGui::MenuItem(Label.c_str(), nullptr, IsCurrent) && !IsCurrent) {
                if (auto R = SelectRenderer(Name); !R)
                    LogError("Renderer selection failed:\n{}", R.error().ToString());
                break;
            }
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

} // namespace SoulEngine
