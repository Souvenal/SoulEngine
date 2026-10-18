module;

#include <toml++/toml.h>

module Core:Util.CVar.impl;

import :Util.CVar;
import :Util.Logging;

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

/// @brief Apply a TOML leaf when the name is registered under type T.
/// @return true when registered under T (value applied, or a
///         type-mismatch warning emitted); false when not.
template <CVarValue T>
auto ApplyAs(CVarManager& Manager, const String& Name, const toml::node& Node) -> bool {
    if (!Manager.Find<T>(Name))
        return false;
    if (auto Converted = ConvertTomlNode<T>(Node))
        Manager.SetByName<T>(Name, *Converted);
    else
        LogWarning("Config key '{}' value does not match CVar type {}; value ignored", Name, CVarTypeName<T>());
    return true;
}

auto CVarManager::ApplyValue(const String& Name, const toml::node& Node) -> void {
    if (ApplyAs<bool>(*this, Name, Node) || ApplyAs<Int32>(*this, Name, Node) || ApplyAs<Float32>(*this, Name, Node) ||
        ApplyAs<String>(*this, Name, Node))
        return;
    LogWarning("Config key '{}' has no matching registered CVar; value ignored", Name);
}

} // namespace SoulEngine
