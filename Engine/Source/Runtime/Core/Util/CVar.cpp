module;

#include <toml++/toml.h>

module Core.Util.CVar;

import Core.Util.Logging;

namespace SoulEngine {

/// @brief Display name of a CVar value type, for diagnostics only.
template <CVarValue T>
consteval auto CVarTypeName() -> StringView {
    if constexpr (std::same_as<T, bool>)
        return "Bool";
    else if constexpr (std::same_as<T, Int32>)
        return "Int32";
    else if constexpr (std::same_as<T, Float32>)
        return "Float32";
    else
        return "String";
}

/// @brief Convert a TOML leaf to a CVar value.  Integers undergo a range
///        check for Int32; Float32 also accepts TOML integers.
template <CVarValue T>
[[nodiscard]] auto ConvertTomlNode(const toml::node& Node) -> std::optional<T> {
    if constexpr (std::same_as<T, bool>) {
        return Node.value<bool>();
    } else if constexpr (std::same_as<T, Int32>) {
        if (const auto V = Node.value<std::int64_t>();
            V && *V >= std::numeric_limits<Int32>::min() && *V <= std::numeric_limits<Int32>::max())
            return std::optional<T>{static_cast<Int32>(*V)};
        return std::nullopt;
    } else if constexpr (std::same_as<T, Float32>) {
        if (const auto V = Node.value<double>())
            return std::optional<T>{static_cast<Float32>(*V)};
        if (const auto V = Node.value<std::int64_t>())
            return std::optional<T>{static_cast<Float32>(*V)};
        return std::nullopt;
    } else {
        return Node.value<String>();
    }
}

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
    std::vector<String> Diagnostics;
    {
        std::unique_lock Lock{m_RegistryMutex};
        Diagnostics.swap(m_PendingDiagnostics);
    }
    for (const auto& Diagnostic : Diagnostics)
        LogError("{}", Diagnostic);
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
