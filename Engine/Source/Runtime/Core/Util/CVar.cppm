module;

#include <toml++/toml.h>

export module Core.Util.CVar;

import Core.Util.Types;
import Core.Util.Error;
import Core.Util.Singleton;
import std;

/// @file
/// @brief Console variables (CVars): runtime-mutable, globally registered
///        configuration values.
///
/// Modeled on Unreal's TAutoConsoleVariable / the vkguide CVarSystem, adapted to this codebase's conventions
/// (C++23 modules, no exceptions, type aliases).
///
/// @section cvar_design Design decisions
///
///  1. **Registration via namespace-scope objects.**
///     A `CVar<T>` object constructed at namespace scope self-registers
///     with CVarManager during static initialization.  There is no central
///     registration function and no mirror config struct: the dotted name
///     (e.g. "Render.UseCSM") is the single canonical identifier.
///
///  2. **The registering object is also the fast handle.**
///     Its constructor resolves its registry entry once; Get()/Set() then
///     touch the entry directly with zero lookup.  Code in other modules
///     uses CVarManager::Find<T>(name) / SetByName<T>(name, value), which
///     return std::optional / bool — a name not registered under type T
///     yields nullopt / false with no logging side effects.  The registry
///     key is (name, type): the same name may exist under several types.
///
///  3. **Defaults live at the registration site.**
///     The constructor's default value is the one-and-only default; the
///     TOML config file overrides it at startup via
///     CVarManager::LoadConfig().  Consumers never spell a fallback.
///
///  4. **Every CVar is runtime-mutable.**
///     Consumption timing declares when readers use a value: Runtime readers poll continuously; Startup
///     readers consume it during startup. Later writes update the stored value for either timing.
///     bool/Int32/Float32 values are atomic; String values sit behind a shared_mutex.
///
///  5. **Static-init diagnostics are deferred.**
///     Registration runs before LogManager::Init, so duplicate-name
///     reports are queued and flushed (LogError) by LoadConfig() /
///     FlushPendingDiagnostics() once logging is live.
///
///  @warning Linkage: registration objects in module interface units must
///     have MODULE linkage (a plain non-exported namespace-scope
///     declaration), NOT internal linkage (anonymous namespace).  An
///     internal-linkage variable referenced from exported inline/template
///     code is silently COPIED into importing TUs (the importer cannot
///     reference the original symbol), and the copy's dynamic initializer
///     runs again — producing a duplicate registration.
///
///  @warning Dead-strip hazard: a translation unit containing ONLY a CVar
///     registration and no referenced symbols may be discarded by the
///     linker, silently dropping the registration.  Keep registrations in
///     translation units that also contain used code.  (Module-partition
///     object files are linked whole by the xmake moduleonly targets, so
///     this mainly matters for future static-library splits.)

namespace SoulEngine {

/// @brief Value types supported by the CVar system.
export template <typename T>
concept CVarValue =
    std::same_as<T, bool> || std::same_as<T, Int32> || std::same_as<T, Float32> || std::same_as<T, String>;

/// @brief When a CVar's value is consumed.
export enum class CVarTiming : Uint8 {
    Unknown = 0,
    Runtime, ///< Readers poll the current value while running.
    Startup, ///< Readers consume the value during startup; later edits require restart.
};

/// @brief Owned copy of one CVar record, usable on any thread after collection.
export template <CVarValue T>
struct CVarSnapshot {
    String     Name;
    String     Description;
    T          Value;
    CVarTiming Timing = CVarTiming::Runtime;
};

// Registry internals (module linkage: invisible outside Core, reachable from the exported templates'
// instantiations in consumer TUs)

/// @brief One registry entry: description plus the value itself.
/// @details Entries live in unordered_map nodes, whose addresses are stable
///          across rehashes — that stability is what lets CVar<T> handles
///          cache an entry pointer at registration.  Arithmetic values are
///          atomic; each String entry owns a shared_mutex guarding its value.
template <CVarValue T>
struct CVarEntry {
    using ValueStorage = std::atomic<T>;

    String       Description;
    ValueStorage Value;
    CVarTiming   Timing = CVarTiming::Runtime;
};
template <>
struct CVarEntry<String> {
    using ValueStorage = String;

    String                    Description;
    ValueStorage              Value;
    CVarTiming                Timing = CVarTiming::Runtime;
    mutable std::shared_mutex Mutex; ///< Guards this entry's String value.
};

/// @brief Per-type registry.  Type mismatch needs no tag or check: a name
///        registered under another type simply is not in this map.
template <CVarValue T>
using CVarRegistry = std::unordered_map<String, CVarEntry<T>>;

/// @brief Central registry of all console variables.
///
/// Registration happens implicitly at static-init time via CVar<T> constructors. Startup then applies the
/// TOML config through LoadConfig(): the file's nested tables are flattened into dotted names (`[RHI.Vulkan]
/// Validation` → "RHI.Vulkan.Validation") and each leaf value is written to the matching CVar. Keys with no
/// registered CVar and values of the wrong type are logged as warnings and skipped.
///
/// Registration completes during single-threaded startup before concurrent access begins. Registry
/// structure and entry metadata stay fixed while values are read and written from any thread.
export class CVarManager final : public Singleton<CVarManager> {
    friend class Singleton<CVarManager>;

    CVarManager() = default;

  public:
    /// @brief Look up a CVar's current value by name.
    /// @return The value, or std::nullopt when the name is not registered
    ///         under type T (whether unknown or registered under another
    ///         type).  Never logs.
    template <CVarValue T>
    [[nodiscard]] auto Find(StringView Name) -> std::optional<T> {
        const auto& Registry = Entries<T>();
        const auto  It       = Registry.find(String{Name});
        if (It == Registry.end())
            return std::nullopt;
        return LoadEntry(It->second);
    }

    /// @brief Copy all records of type T with their current values, sorted by dotted name.
    template <CVarValue T>
    [[nodiscard]] auto Collect() -> std::vector<CVarSnapshot<T>> {
        std::vector<CVarSnapshot<T>> Records;
        const auto&                  Registry = Entries<T>();
        Records.reserve(Registry.size());
        for (const auto& [Name, Entry] : Registry)
            Records.push_back(
                {.Name = Name, .Description = Entry.Description, .Value = LoadEntry(Entry), .Timing = Entry.Timing});
        std::ranges::sort(Records, {}, &CVarSnapshot<T>::Name);
        return Records;
    }

    /// @brief Write a CVar's value by name.
    /// @return false when the name is not registered under type T.
    template <CVarValue T>
    auto SetByName(StringView Name, const T& Value) -> bool {
        auto&      Registry = Entries<T>();
        const auto It       = Registry.find(String{Name});
        if (It == Registry.end())
            return false;
        StoreEntry(It->second, Value);
        return true;
    }

    /// @brief Parse a TOML config file and apply every leaf value to the
    ///        CVar whose dotted name matches its table path.
    /// @retval std::unexpected Parse error (e.g. file not found or invalid TOML).
    [[nodiscard]] auto LoadConfig(const Path& FilePath) -> std::expected<void, ErrorMessage>;

    /// @brief Emit any diagnostics queued during static-init registration
    ///        (duplicate names).  Safe to call multiple times.
    auto FlushPendingDiagnostics() -> void;

    /// @brief Register a CVar and return its registry entry.  Infallible:
    ///        the registry key is (name, type) — the same name under another
    ///        type is simply a different CVar.  A same-type duplicate is
    ///        first-wins (the new handle shares the existing entry) and is
    ///        reported via the deferred diagnostic queue.
    /// @return Reference to the registry entry (stable for the process
    ///         lifetime — unordered_map nodes never relocate).
    template <CVarValue T>
    auto
    Register(StringView Name, StringView Description, const T& DefaultValue, CVarTiming Timing = CVarTiming::Runtime)
        -> CVarEntry<T>& {
        auto&        Registry = Entries<T>();
        const String NameStr{Name};
        const auto [It, Inserted] = Registry.try_emplace(NameStr, String{Description}, DefaultValue, Timing);
        if (!Inserted)
            m_PendingDiagnostics.push_back(
                Format("Duplicate CVar registration '{}' ignored (first registration wins)", NameStr));
        return It->second;
    }

    /// @brief The single read path for all CVar values.
    template <CVarValue T>
    [[nodiscard]] auto LoadEntry(const CVarEntry<T>& Entry) -> T {
        if constexpr (std::same_as<T, String>) {
            std::shared_lock Lock{Entry.Mutex};
            return Entry.Value;
        } else {
            return Entry.Value.load(std::memory_order_relaxed);
        }
    }

    /// @brief The single write path for all CVar values.
    template <CVarValue T>
    auto StoreEntry(CVarEntry<T>& Entry, const T& Value) -> void {
        if constexpr (std::same_as<T, String>) {
            std::unique_lock Lock{Entry.Mutex};
            Entry.Value = Value;
        } else {
            Entry.Value.store(Value, std::memory_order_relaxed);
        }
    }

  private:
    template <CVarValue T>
    auto Entries() -> CVarRegistry<T>& {
        return std::get<CVarRegistry<T>>(m_Registries);
    }

    /// @brief Recursive TOML walk: tables descend with a dotted prefix,
    ///        leaves are applied as CVar values.
    auto ApplyTable(const toml::table& Table, const String& Prefix) -> void;

    auto ApplyValue(const String& Name, const toml::node& Node) -> void;

    std::tuple<CVarRegistry<bool>, CVarRegistry<Int32>, CVarRegistry<Float32>, CVarRegistry<String>> m_Registries;
    std::vector<String> m_PendingDiagnostics;
};

/// @brief A console variable: namespace-scope registration object and
///        zero-lookup access handle.
///
/// Register by constructing at namespace scope in any translation unit: ~~~{.cpp}
///     CVar<bool> CVarRenderUseCSM{ "Render.UseCSM", true,
///         "Use cascaded shadow maps instead of a single shadow map" };
///
///     if (CVarRenderUseCSM.Get()) { /* add CSM pass */ }
/// ~~~
///
/// The value is runtime-mutable from any thread; readers poll via Get().
export template <CVarValue T>
class CVar {
  public:
    /// @brief Construct and self-register with CVarManager.
    /// @param Name         Canonical dotted name, "Module.Feature" style,
    ///                     isomorphic to the TOML table path.
    /// @param DefaultValue The one-and-only default; the TOML config may
    ///                     override it at startup.
    /// @param Description  Human-readable explanation for future UI and logs.
    /// @param Timing       When readers consume the stored value.
    CVar(StringView Name, const T& DefaultValue, StringView Description, CVarTiming Timing = CVarTiming::Runtime)
        : m_Entry(CVarManager::Get().Register<T>(Name, Description, DefaultValue, Timing)), m_Name(Name) {}

    /// @brief Read the current value.  Safe to call from any thread.
    [[nodiscard]] auto Get() const -> T {
        return CVarManager::Get().LoadEntry(m_Entry);
    }

    /// @brief Write a new value.  Safe to call from any thread; readers
    ///        observe it on their next Get().
    auto Set(const T& Value) -> void {
        CVarManager::Get().StoreEntry(m_Entry, Value);
    }

    /// @brief The canonical dotted name this CVar registered under.
    [[nodiscard]] auto Name() const -> StringView {
        return m_Name;
    }

  private:
    CVarEntry<T>& m_Entry; ///< Registry-owned entry; registration is infallible.
    StringView    m_Name;
};

} // namespace SoulEngine
