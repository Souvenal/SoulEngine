module;
#include <toml++/toml.h>

module Core.Util.CVar;

import Core.Util.Logging;
import std;

namespace SoulEngine {

auto CVarManager::LoadConfig(const Path& FilePath) -> std::expected<void, ErrorMessage> {
    // Registration failures recorded during static init are fatal: surface them here, the first point
    // where Launch's normal error path can act on them, and skip the sweep entirely.
    if (!m_RegistrationErrors.empty()) {
        String Joined;
        for (const auto& Error : m_RegistrationErrors)
            Joined += Format("\n  - {}", Error);
        return std::unexpected(
            ErrorMessage(Format("CVar registration failed with {} error(s):{}", m_RegistrationErrors.size(), Joined)));
    }

    auto Result = toml::parse_file(FilePath.string());
    if (!Result) {
        auto& Err = Result.error();
        return std::unexpected(ErrorMessage(Format("Failed to parse '{}':\n  {} (at line {}, column {})",
                                                   FilePath.string(),
                                                   Err.description(),
                                                   Err.source().begin.line,
                                                   Err.source().begin.column)));
    }

    auto Table = std::move(Result).table();
    LogInfo("Loaded config file: {}", FilePath.string());

    ApplyTable(Table, "");
    return {};
}

auto CVarManager::ApplyTable(const toml::table& Table, const String& Prefix) -> void {
    for (const auto& [Key, Value] : Table) {
        const String FullName = Prefix.empty() ? String{Key.str()} : Format("{}.{}", Prefix, StringView{Key.str()});
        if (const auto* SubTable = Value.as_table())
            ApplyTable(*SubTable, FullName);
        else
            ApplyValue(FullName, Value);
    }
}

auto CVarManager::ApplyValue(const String& Name, const toml::node& Node) -> void {
    const auto It = m_Entries.find(Name);
    if (It == m_Entries.end()) {
        LogWarning("Config key '{}' has no matching registered CVar; value ignored", Name);
        return;
    }
    It->second->TryApplyToml(Name, Node);
}

} // namespace SoulEngine
