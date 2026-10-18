module;

#include <toml++/toml.h>

module Core:Util.CVar.impl;

import :Util.CVar;

namespace SoulEngine {

auto CVarManager::LoadConfig(const Path& FilePath) -> std::expected<void, ErrorMessage> {
    // Registration diagnostics were queued during static init, before the logger existed; LoadConfig is
    // the first point where logging is guaranteed live, so flush them here.
    FlushPendingDiagnostics();

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

auto CVarManager::FlushPendingDiagnostics() -> void {
    for (const auto& Diagnostic : m_PendingDiagnostics)
        LogError("{}", Diagnostic);
    m_PendingDiagnostics.clear();
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
    if (ApplyAs<bool>(Name, Node) || ApplyAs<Int32>(Name, Node) || ApplyAs<Float32>(Name, Node) ||
        ApplyAs<String>(Name, Node))
        return;
    LogWarning("Config key '{}' has no matching registered CVar; value ignored", Name);
}

} // namespace SoulEngine
