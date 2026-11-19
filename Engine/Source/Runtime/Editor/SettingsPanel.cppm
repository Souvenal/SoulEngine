export module Editor.SettingsPanel;

import Core;
import EditorCommands;
import std;

namespace SoulEngine {

/// @brief Register the shared Boolean CVar mutation command.
export [[nodiscard]] auto RegisterSettingsCommand(CommandDispatcher& Commands) -> std::expected<Uint64, ErrorMessage>;

/// @brief Register the standalone boolean CVar settings panel.
export auto RegisterSettingsPanel(CommandDispatcher& Commands) -> void;

} // namespace SoulEngine
