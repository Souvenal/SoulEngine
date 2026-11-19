module;

module Editor.UIManager;

import std;

namespace SoulEngine {

auto UIManager::SubmitPanelToggle(CommandDispatcher& Commands, StringView Panel)
    -> std::expected<Uint64, ErrorMessage> {
    return Commands.SubmitByUser("editor.panel.toggle", String(Panel));
}
auto UIManager::SubmitPanelClose(CommandDispatcher& Commands, StringView Panel) -> std::expected<Uint64, ErrorMessage> {
    return Commands.SubmitByUser("editor.panel.close", String(Panel));
}
auto UIManager::RegisterPanelCommands(CommandDispatcher& Commands)
    -> std::expected<UIPanelCommandRegistrations, ErrorMessage> {
    const auto ValidatePanel = [this](CommandContext& Context, const String& Panel) -> bool {
        if ((Panel == "Settings" || Panel == "Debug.ShowMaterials") && m_Entries.contains(Panel))
            return true;
        Context.Fail("stale_target", Format("Panel '{}' is unavailable", Panel), CommandJson{{"panel", Panel}});
        return false;
    };
    auto Toggle = Commands.Register(CommandDescriptor{.Name           = "editor.panel.toggle",
                                                      .Parameters     = {{.Name = "panel"}},
                                                      .IsAgentExposed = true,
                                                      .Category       = CommandCategory::Input},
                                    [this, ValidatePanel](CommandContext& Context, String Panel) -> void {
                                        if (ValidatePanel(Context, Panel))
                                            ToggleUI(Panel);
                                    });
    if (!Toggle)
        return std::unexpected(Toggle.error().Append("Failed to register panel toggle command"));
    auto Close = Commands.Register(CommandDescriptor{.Name       = "editor.panel.close",
                                                     .Parameters = {{.Name = "panel"}},
                                                     .Category   = CommandCategory::Input},
                                   [this, ValidatePanel](CommandContext& Context, String Panel) -> void {
                                       if (ValidatePanel(Context, Panel))
                                           CloseUI(Panel);
                                   });
    if (!Close) {
        Commands.Retire(*Toggle);
        return std::unexpected(Close.error().Append("Failed to register panel close command"));
    }
    return UIPanelCommandRegistrations{.Toggle = *Toggle, .Close = *Close};
}
auto UIManager::Register(std::string name, std::function<void()> callback, bool defaultShow) -> void {
    m_Entries[std::move(name)] = {std::move(callback), defaultShow};
}

auto UIManager::OpenUI(const std::string& name) -> void {
    if (auto it = m_Entries.find(name); it != m_Entries.end()) {
        it->second.Show = true;
    }
}

auto UIManager::CloseUI(const std::string& name) -> void {
    if (auto it = m_Entries.find(name); it != m_Entries.end()) {
        it->second.Show = false;
    }
}

auto UIManager::ToggleUI(const std::string& name) -> void {
    if (auto it = m_Entries.find(name); it != m_Entries.end()) {
        it->second.Show = !it->second.Show;
    }
}

auto UIManager::IsUIShowing(const std::string& name) const -> bool {
    if (auto it = m_Entries.find(name); it != m_Entries.end()) {
        return it->second.Show;
    }
    return false;
}

auto UIManager::GetAll() -> std::unordered_map<std::string, UIEntry>& {
    return m_Entries;
}

auto UIManager::GetAll() const -> const std::unordered_map<std::string, UIEntry>& {
    return m_Entries;
}

} // namespace SoulEngine
