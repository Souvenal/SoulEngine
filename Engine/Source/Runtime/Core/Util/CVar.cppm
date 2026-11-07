module;

#include <toml++/toml.h>

export module Core.Util.CVar;

import Core.Util.Types;
import Core.Util.Error;
import Core.Util.Logging;
import Core.Util.Singleton;
import std;

import magic_enum;

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
///     yields nullopt / false with no logging side effects. SetByName also
///     returns false when a value violates declared options. The registry
///     key is (name, type): the same name may exist under several types.
///
///  3. **Defaults live at the registration site.**
///     The constructor's default value is the one-and-only default; the
///     TOML config file overrides it at startup via
///     CVarManager::LoadConfig().  Consumers never spell a fallback.
///     Option-bearing CVars also declare ordered options. Every write
///     path enforces it. An invalid registration default uses the first
///     option with a deferred warning; an empty option set rejects all writes.
///
///  4. **Every CVar is runtime-mutable.**
///     Consumption timing declares when readers use a value: Runtime readers poll continuously; Startup
///     readers consume it during startup. Later writes update the stored value for either timing.
///     bool/Int32/Float32 values are atomic; String values sit behind a shared_mutex.
///
///  5. **Static-init diagnostics are deferred.**
///     Registration runs before LogManager::Init, so registration
///     reports are queued and replayed with their recorded severity by
///     LoadConfig() / FlushPendingDiagnostics() once logging is live.
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

template <>
struct CVarSnapshot<String> {
    /// @brief Owned copy of a String CVar record, usable on any thread after collection.  Option-bearing
    ///        records carry their ordered legal values; Options is empty for free-form String CVars.
    String              Name;
    String              Description;
    String              Value;
    CVarTiming          Timing  = CVarTiming::Runtime;
    std::vector<String> Options = {}; ///< Ordered legal values.
};

/// @brief One registry entry: description, option metadata, and the value itself.
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
    CVarTiming                Timing          = CVarTiming::Runtime;
    std::vector<String>       Options         = {};
    bool                      IsOptionBearing = false;
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
/// Validation` → "RHI.Vulkan.Validation") and each leaf value is written to the matching CVar.
/// Unknown keys and values with the wrong type or outside declared options produce warnings and are skipped.
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
        for (const auto& [Name, Entry] : Registry) {
            if constexpr (std::same_as<T, String>) {
                Records.push_back({.Name        = Name,
                                   .Description = Entry.Description,
                                   .Value       = LoadEntry(Entry),
                                   .Timing      = Entry.Timing,
                                   .Options     = Entry.Options});
            } else {
                Records.push_back({.Name        = Name,
                                   .Description = Entry.Description,
                                   .Value       = LoadEntry(Entry),
                                   .Timing      = Entry.Timing});
            }
        }
        std::ranges::sort(Records, {}, &CVarSnapshot<T>::Name);
        return Records;
    }

    /// @brief Write a CVar's value by name.
    /// @return false when the name is not registered under type T or the value is outside its options.
    template <CVarValue T>
    auto SetByName(StringView Name, const T& Value) -> bool {
        auto&      Registry = Entries<T>();
        const auto It       = Registry.find(String{Name});
        if (It == Registry.end())
            return false;
        if (!StoreEntry(It->second, Value)) {
            LogWarning("CVar '{}' value '{}' is outside its declared options; value ignored", Name, Value);
            return false;
        }
        return true;
    }

    /// @brief Parse a TOML config file and apply every leaf value to the
    ///        CVar whose dotted name matches its table path.
    /// @retval std::unexpected Parse error (e.g. file not found or invalid TOML).
    [[nodiscard]] auto LoadConfig(const Path& FilePath) -> std::expected<void, ErrorMessage>;

    /// @brief Emit any diagnostics queued during static-init registration.
    ///        Safe to call multiple times.
    auto FlushPendingDiagnostics() -> void;

    /// @brief Register a CVar and return its registry entry.  Infallible:
    ///        the registry key is (name, type) — the same name under another
    ///        type is simply a different CVar.  A same-type duplicate is
    ///        first-wins (the new handle shares the existing entry) and is
    ///        reported via the deferred diagnostic queue.
    /// @return Reference to the registry entry (stable for the process
    ///         lifetime — unordered_map nodes never relocate).
    template <CVarValue T>
    [[nodiscard]] auto
    Register(StringView Name, StringView Description, const T& DefaultValue, CVarTiming Timing = CVarTiming::Runtime)
        -> CVarEntry<T>& {
        return RegisterImpl<T>(Name, Description, DefaultValue, Timing, {}, false);
    }

    /// @brief Register an option-bearing String CVar and return its registry entry.  The default is
    ///        rejected when it lies outside Options; the first option becomes the initial value and a
    ///        warning is queued.  An empty Options list queues an error and rejects every write.
    /// @return Reference to the registry entry (stable for the process
    ///         lifetime — unordered_map nodes never relocate).
    template <CVarValue T>
        requires std::same_as<T, String>
    [[nodiscard]] auto Register(StringView          Name,
                                StringView          Description,
                                const String&       DefaultValue,
                                CVarTiming          Timing,
                                std::vector<String> Options) -> CVarEntry<String>& {
        return RegisterImpl<T>(Name, Description, DefaultValue, Timing, std::move(Options), true);
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

    /// @brief Validate and write a CVar value.
    /// @return true when the value satisfies the declared options and is stored.
    template <CVarValue T>
    [[nodiscard]] auto StoreEntry(CVarEntry<T>& Entry, const T& Value) -> bool {
        if constexpr (std::same_as<T, String>) {
            if (Entry.IsOptionBearing && !IsAllowedByOptions(Entry.Options, Value))
                return false;
            std::unique_lock Lock{Entry.Mutex};
            Entry.Value = Value;
        } else {
            Entry.Value.store(Value, std::memory_order_relaxed);
        }
        return true;
    }

  private:
    struct CVarDiagnostic {
        String   Message;
        LogLevel Level = LogLevel::Info;
    };

    template <CVarValue T>
    [[nodiscard]] static auto IsAllowedByOptions(const std::vector<T>& Options, const T& Value) -> bool {
        return std::ranges::find(Options, Value) != Options.end();
    }

    template <CVarValue T>
    [[nodiscard]] auto RegisterImpl(StringView     Name,
                                    StringView     Description,
                                    const T&       DefaultValue,
                                    CVarTiming     Timing,
                                    std::vector<T> Options,
                                    bool           IsOptionBearing) -> CVarEntry<T>& {
        auto&        Registry = Entries<T>();
        const String NameStr{Name};
        auto         It       = Registry.end();
        bool         Inserted = false;
        if constexpr (std::same_as<T, String>) {
            T InitialValue = DefaultValue;
            if (IsOptionBearing) {
                if (Options.empty()) {
                    m_PendingDiagnostics.push_back(
                        {.Message = Format("CVar '{}' has no legal options; all writes rejected", NameStr),
                         .Level   = LogLevel::Error});
                } else if (!IsAllowedByOptions(Options, DefaultValue)) {
                    InitialValue = Options.front();
                    m_PendingDiagnostics.push_back(
                        {.Message = Format(
                             "CVar '{}' default is outside its declared options; using '{}'", NameStr, InitialValue),
                         .Level = LogLevel::Warning});
                }
            }
            std::tie(It, Inserted) = Registry.try_emplace(
                NameStr, String{Description}, InitialValue, Timing, std::move(Options), IsOptionBearing);
        } else {
            std::tie(It, Inserted) = Registry.try_emplace(NameStr, String{Description}, DefaultValue, Timing);
        }
        if (!Inserted) {
            m_PendingDiagnostics.push_back(
                {.Message = Format("Duplicate CVar registration '{}' ignored (first registration wins)", NameStr),
                 .Level   = LogLevel::Error});
        }
        return It->second;
    }

    template <CVarValue T>
    auto Entries() -> CVarRegistry<T>& {
        return std::get<CVarRegistry<T>>(m_Registries);
    }

    /// @brief Recursive TOML walk: tables descend with a dotted prefix,
    ///        leaves are applied as CVar values.
    auto ApplyTable(const toml::table& Table, const String& Prefix) -> void;

    auto ApplyValue(const String& Name, const toml::node& Node) -> void;

    std::tuple<CVarRegistry<bool>, CVarRegistry<Int32>, CVarRegistry<Float32>, CVarRegistry<String>> m_Registries;
    std::vector<CVarDiagnostic> m_PendingDiagnostics;
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

    /// @brief Construct and self-register with an ordered set of legal values.
    CVar(StringView     Name,
         const T&       DefaultValue,
         StringView     Description,
         std::vector<T> Options,
         CVarTiming     Timing = CVarTiming::Runtime)
        requires std::same_as<T, String>
        : m_Entry(CVarManager::Get().Register<T>(Name, Description, DefaultValue, Timing, std::move(Options))),
          m_Name(Name) {}

    /// @brief Read the current value.  Safe to call from any thread.
    [[nodiscard]] auto Get() const -> T {
        return CVarManager::Get().LoadEntry(m_Entry);
    }

    /// @brief Write a new value.  Safe to call from any thread; readers
    ///        observe it on their next Get().
    auto Set(const T& Value) -> void {
        if (!CVarManager::Get().StoreEntry(m_Entry, Value))
            LogWarning("CVar '{}' value '{}' is outside its declared options; value ignored", m_Name, Value);
    }

    /// @brief The canonical dotted name this CVar registered under.
    [[nodiscard]] auto Name() const -> StringView {
        return m_Name;
    }

  private:
    CVarEntry<T>& m_Entry; ///< Registry-owned entry; registration is infallible.
    StringView    m_Name;
};

/// @brief Strongly typed enum setting with discrete CVar options.
///
/// Construct at namespace scope with a default and explicit ordered options, then use Get to poll the current
/// value and Set to write a named enumerator. Get and Set are safe to call from any thread.
export template <typename E>
    requires std::is_enum_v<E>
class EnumCVar {
  public:
    /// @brief Register reflected enum names and retain the typed default as a read fallback.
    EnumCVar(StringView               Name,
             E                        DefaultValue,
             StringView               Description,
             std::initializer_list<E> Options,
             CVarTiming               Timing = CVarTiming::Runtime)
        : m_Default(DefaultValue),
          m_Value(Name,
                  String{magic_enum::enum_name(DefaultValue)},
                  Description,
                  OptionNames(DefaultValue, Options),
                  Timing) {}

    /// @brief Read the current typed value from any thread.
    [[nodiscard]] auto Get() const -> E {
        return magic_enum::enum_cast<E>(m_Value.Get()).value_or(m_Default);
    }

    /// @brief Write a named enum value; option validation is shared with String CVars.
    auto Set(E Value) -> void {
        const auto Name = magic_enum::enum_name(Value);
        if (Name.empty()) {
            LogWarning("CVar '{}' received an unnamed enum value; value ignored", m_Value.Name());
            return;
        }
        m_Value.Set(String{Name});
    }

    /// @brief The canonical dotted name shared with the String registry entry.
    [[nodiscard]] auto Name() const -> StringView {
        return m_Value.Name();
    }

  private:
    [[nodiscard]] static auto OptionNames(E DefaultValue, std::initializer_list<E> Options) -> std::vector<String> {
        std::vector<String> Result = {};
        if (Options.size() == 0 || magic_enum::enum_name(DefaultValue).empty())
            return Result;
        Result.reserve(Options.size());
        for (const E Option : Options) {
            const auto Name = magic_enum::enum_name(Option);
            if (Name.empty())
                return {};
            Result.emplace_back(Name);
        }
        return Result;
    }

    E            m_Default;
    CVar<String> m_Value;
};

} // namespace SoulEngine
