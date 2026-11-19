#pragma once

#include <entt/entt.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_threaded_rendering.h>

import std;
import Core;
import Editor;
import EditorCommands;

namespace SoulEngine {

struct HeadlessEditor {
    explicit HeadlessEditor(std::size_t Capacity = 256) : Commands(Capacity) {
        EditorValue.Initialize();
        auto& IO              = ImGui::GetIO();
        IO.IniFilename        = nullptr;
        IO.DisplaySize        = ImVec2(800, 600);
        IO.DeltaTime          = 1.0F / 60.0F;
        unsigned char* Pixels = nullptr;
        int            Width  = 0;
        int            Height = 0;
        IO.Fonts->GetTexDataAsRGBA32(&Pixels, &Width, &Height);
        IO.Fonts->SetTexID(ImTextureID(1));
        IO.Fonts->TexData->SetStatus(ImTextureStatus_OK);
    }
    ~HeadlessEditor() {
        EditorValue.UnbindScene();
        EditorValue.Shutdown();
    }

    [[nodiscard]] auto Bind() -> std::expected<void, ErrorMessage> {
        return EditorValue.BindCommands(Commands);
    }
    auto Frame(float X = -100.0F, float Y = -100.0F, bool Down = false) -> void {
        auto& IO = ImGui::GetIO();
        IO.AddMousePosEvent(X, Y);
        IO.AddMouseButtonEvent(0, Down);
        ImDrawDataSnapshot Snapshot;
        EditorValue.Tick(1.0F / 60.0F, Snapshot);
    }
    auto Click(ImVec2 Position) -> void {
        Frame(Position.x, Position.y, false);
        Frame(Position.x, Position.y, true);
        Frame(Position.x, Position.y, false);
    }
    auto DispatchAction() -> void {
        Commands.ExecutePhase(CommandPhase::Action, ++ActionFrame);
    }

    CommandDispatcher Commands;
    Editor            EditorValue;
    Uint64            ActionFrame = 0;
};

[[nodiscard]] inline auto MainMenuX(std::initializer_list<const char*> Items, StringView Target) -> float {
    const auto& Style = ImGui::GetStyle();
    float       X     = Style.WindowPadding.x;
    for (const auto* Item : Items) {
        const float Width = ImGui::CalcTextSize(Item).x + Style.FramePadding.x * 2.0F;
        if (Target == Item)
            return X + Width * 0.5F;
        X += Width + Style.ItemSpacing.x;
    }
    return 0.0F;
}

[[nodiscard]] inline auto FindImGuiWindow(StringView Name) -> ImGuiWindow* {
    for (auto* Window : ImGui::GetCurrentContext()->Windows) {
        if (!Window->Active && !Window->WasActive)
            continue;
        const StringView Candidate(Window->Name);
        if (Candidate == Name ||
            (Candidate.starts_with(Name) && Candidate.size() > Name.size() && Candidate[Name.size()] == '_'))
            return Window;
    }
    return nullptr;
}

[[nodiscard]] inline auto FindImGuiMenu(StringView Name) -> ImGuiWindow* {
    for (auto* Window : ImGui::GetCurrentContext()->Windows) {
        if (!Window->Active && !Window->WasActive)
            continue;
        const StringView Candidate(Window->Name);
        if (Candidate.starts_with(Name) && Candidate.size() > Name.size() + 2 &&
            Candidate.substr(Name.size(), 3) == "###")
            return Window;
    }
    return nullptr;
}

[[nodiscard]] inline auto ClickImGuiMenuRow(HeadlessEditor& Fixture, StringView Name, std::size_t Row) -> bool {
    Fixture.Frame();
    const auto* Window = FindImGuiMenu(Name);
    if (!Window)
        return false;
    const auto& Style       = ImGui::GetStyle();
    const float ItemHeight  = ImGui::GetFrameHeight();
    const float ItemAdvance = ItemHeight + Style.ItemSpacing.y;
    Fixture.Click(ImVec2(Window->Pos.x + Window->WindowPadding.x + ItemHeight * 0.5F,
                         Window->Pos.y + Window->WindowPadding.y + ItemHeight * 0.5F + Row * ItemAdvance));
    return true;
}

[[nodiscard]] inline auto ClickImGuiWindowClose(HeadlessEditor& Fixture, StringView Name) -> bool {
    Fixture.Frame();
    const auto* Window = FindImGuiWindow(Name);
    if (!Window)
        return false;
    const auto& Style      = ImGui::GetStyle();
    const float ButtonSize = ImGui::GetFontSize();
    Fixture.Click(ImVec2(Window->Pos.x + Window->Size.x - Style.FramePadding.x * 2.0F - ButtonSize * 0.5F,
                         Window->Pos.y + Style.FramePadding.y + ButtonSize * 0.5F));
    return true;
}

} // namespace SoulEngine
