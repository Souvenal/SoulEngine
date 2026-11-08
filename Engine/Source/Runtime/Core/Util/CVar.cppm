module;

#include <toml++/toml.h>

export module Core.Util.CVar;

import Core.Util.Types;
import Core.Util.Error;
import Core.Util.Flags;
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
///     key is (name, storage type): enum CVars share the String registry
///     under their reflected enumerator names.
///
///  3. **Defaults live at the registration site.**
///     The constructor's default value is the one-and-only default; the
///     TOML config file overrides it at startup via
///     CVarManager::LoadConfig().  Consumers never spell a fallback.
///     A storage CVar may declare ordered options; every write path
///     enforces them, and an invalid registration default uses the first
///     option with a deferred warning.  Enum CVars do not take option
///     lists: every named enumerator of the enum is legal, enforced
///     through the reflected names in the String registry.
///
///  4. **Every CVar is runtime-mutable.**
///     Consumption timing declares when readers use a value: Runtime readers poll continuously; Startup
///     readers consume it during startup. Later writes update the stored value for either timing.
///     bool/Int32/Float32 values are atomic; String values (including enum CVars) sit behind a shared_mutex.
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

/// @brief Storage types backing the CVar registries: bool, Int32, Float32,
///        and String.
export template <typename T>
concept CVarStorage =
    std::same_as<T, bool> || std::same_as<T, Int32> || std::same_as<T, Float32> || std::same_as<T, String>;

/// @brief Value types a CVar can expose: storage types plus any enum.
///        Enum CVars store their reflected enumerator names in the String
///        registry and convert on Get/Set.
export template <typename T>
concept CVarValue = CVarStorage<T> || std::is_enum_v<T>;

/// @brief The registry storage type backing a CVar value type.
template <CVarValue T>
using CVarStorageOf = std::conditional_t<std::is_enum_v<T>, String, T>;

/// @brief When a CVar's value is consumed.
export enum class CVarTiming : Uint8 {
    Unknown = 0,
    Runtime, ///< Readers poll the current value while running.
    Startup, ///< Readers consume the value during startup; later edits require restart.
};

/// @brief One registry entry: description, option metadata, and the value itself.
/// @details Entries live in unordered_map nodes, whose addresses are stable
///          across rehashes — that stability is what lets CVar<T> handles
///          cache an entry pointer at registration and lets callers hold
///          entry references across frames.  Registration completes during
///          single-threaded startup; afterwards options and timing are
///          immutable and only the value is written concurrently, so a
///          reader that reads one value at a time needs no additional
///          synchronization.  Arithmetic values are atomic; each String
///          entry owns a shared_mutex guarding its value.
export template <CVarStorage T>
struct CVarEntry {
    using ValueStorage = std::atomic<T>;

    String         Description;
    ValueStorage   Value;
    CVarTiming     Timing  = CVarTiming::Runtime;
    std::vector<T> Options = {}; ///< Ordered legal values; empty accepts every value.

    CVarEntry(String DescriptionValue, T InitialValue, CVarTiming EntryTiming, std::vector<T> EntryOptions)
        : Description{std::move(DescriptionValue)},
          Value{InitialValue},
          Timing{EntryTiming},
          Options{std::move(EntryOptions)} {}
};
template <>
struct CVarEntry<String> {
    using ValueStorage = String;

    String                    Description;
    ValueStorage              Value;
    CVarTiming                Timing  = CVarTiming::Runtime;
    std::vector<String>       Options = {}; ///< Ordered legal values; empty accepts every value.
    mutable std::shared_mutex Mutex;        ///< Guards this entry's String value.

    CVarEntry(String DescriptionValue, String InitialValue, CVarTiming EntryTiming, std::vector<String> EntryOptions)
        : Description{std::move(DescriptionValue)},
          Value{std::move(InitialValue)},
          Timing{EntryTiming},
          Options{std::move(EntryOptions)} {}
};

/// @brief Per-type registry.  Type mismatch needs no tag or check: a name
///        registered under another type simply is not in this map.
template <CVarStorage T>
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
    ///         type) or the stored string does not parse back to T.  Never
    ///         logs.
    template <CVarValue T>
    [[nodiscard]] auto Find(StringView Name) -> std::optional<T> {
        using Storage        = CVarStorageOf<T>;
        const auto& Registry = Entries<Storage>();
        const auto  It       = Registry.find(String{Name});
        if (It == Registry.end())
            return std::nullopt;
        return FromStorageValue<T>(LoadEntry(It->second));
    }

    /// @brief Visit every CVar registered under storage type T.
    /// @param Visitor Invoked as Visitor(Name, Entry) for each entry; the
    ///                entry reference stays valid for the process lifetime.
    ///                Read values through LoadEntry; options and timing are
    ///                immutable after registration.  Call only after
    ///                registration has completed.
    template <CVarStorage T>
    auto ForEach(auto&& Visitor) -> void {
        for (const auto& [Name, Entry] : Entries<T>())
            Visitor(StringView{Name}, Entry);
    }

    /// @brief Write a CVar's value by name.
    /// @return false when the name is not registered under type T, the enum
    ///         value has no reflected name, or the value is outside its
    ///         declared options.
    template <CVarValue T>
    auto SetByName(StringView Name, const T& Value) -> bool {
        using Storage       = CVarStorageOf<T>;
        auto&      Registry = Entries<Storage>();
        const auto It       = Registry.find(String{Name});
        if (It == Registry.end())
            return false;
        const auto Stored = ToStorageValue(Value);
        if (!Stored) {
            LogWarning("CVar '{}' received an unnamed enum value; value ignored", Name);
            return false;
        }
        if (!StoreEntry(It->second, *Stored)) {
            LogWarning("CVar '{}' value '{}' is outside its declared options; value ignored", Name, *Stored);
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

    /// @brief Register a CVar without options and return its registry entry.
    ///        Infallible: the registry key is (name, type) — the same name
    ///        under another type is simply a different CVar.  A same-type
    ///        duplicate is first-wins (the new handle shares the existing
    ///        entry) and is reported via the deferred diagnostic queue.
    /// @return Reference to the registry entry (stable for the process
    ///         lifetime — unordered_map nodes never relocate).
    /// @brief Register a CVar and return its registry entry.  An empty Options span registers a free-form
    ///        CVar that accepts every value; a non-empty span registers an ordered set of legal values whose
    ///        default is rejected when it lies outside Options — the first option becomes the initial value
    ///        and a warning is queued.  Infallible: the registry key is (name, type) — the same name under
    ///        another type is simply a different CVar.  A same-type duplicate is first-wins (the new handle
    ///        shares the existing entry) and is reported via the deferred diagnostic queue.  Enum CVar
    ///        registration goes through the CVar<E> handle instead.
    /// @return Reference to the registry entry (stable for the process
    ///         lifetime — unordered_map nodes never relocate).
    template <CVarStorage T>
    [[nodiscard]] auto Register(StringView         Name,
                                StringView         Description,
                                const T&           DefaultValue,
                                std::span<const T> Options = {},
                                CVarTiming         Timing  = CVarTiming::Runtime) -> CVarEntry<T>& {
        return RegisterImpl<T>(Name, Description, DefaultValue, Timing, {Options.begin(), Options.end()});
    }

    /// @brief Report a rejected registration and return an inert stand-in entry.  The entry is not
    ///        inserted into any registry, so SetByName on the rejected name fails and a handle bound to it
    ///        never writes; Get falls back to the handle's declared default.  The dummy is shared by all
    ///        rejected registrations of the type — safe, since rejected handles carry no readable state.
    template <CVarStorage T>
    [[nodiscard]] auto RejectedEntry(StringView Name, StringView Reason) -> CVarEntry<T>& {
        static CVarEntry<T> Dummy{String{}, T{}, CVarTiming::Runtime, {}};
        m_PendingDiagnostics.push_back(
            {.Message = Format("CVar '{}' registration rejected: {}", Name, Reason), .Level = LogLevel::Error});
        return Dummy;
    }

    /// @brief The single read path for all CVar values.
    template <CVarStorage T>
    [[nodiscard]] auto LoadEntry(const CVarEntry<T>& Entry) -> T {
        if constexpr (std::same_as<T, String>) {
            std::shared_lock Lock{Entry.Mutex};
            return Entry.Value;
        } else {
            return Entry.Value.load(std::memory_order_relaxed);
        }
    }

    /// @brief Convert a CVar value to its registry storage representation.
    ///        Enums convert to their reflected name; unnamed values yield
    ///        std::nullopt.
    template <CVarValue T>
    [[nodiscard]] static auto ToStorageValue(const T& Value) -> std::optional<CVarStorageOf<T>> {
        if constexpr (std::is_enum_v<T>) {
            const auto Name = magic_enum::enum_name(Value);
            if (Name.empty())
                return std::nullopt;
            return String{Name};
        } else {
            return Value;
        }
    }

    /// @brief Convert a registry storage value to the public CVar type.
    ///        Exposed for CVar handles and traversal callers that read entry
    ///        values directly and need the typed form.
    template <CVarValue T>
    [[nodiscard]] static auto FromStorageValue(const CVarStorageOf<T>& Value) -> std::optional<T> {
        if constexpr (std::is_enum_v<T>)
            return magic_enum::enum_cast<T>(Value);
        else
            return Value;
    }

    /// @brief Validate and write a CVar value.
    /// @return true when the value satisfies the declared options and is stored.
    template <CVarStorage T>
    [[nodiscard]] auto StoreEntry(CVarEntry<T>& Entry, const T& Value) -> bool {
        if (!Entry.Options.empty() && !IsAllowedByOptions(Entry.Options, Value))
            return false;
        if constexpr (std::same_as<T, String>) {
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

    template <CVarStorage T>
    [[nodiscard]] static auto IsAllowedByOptions(const std::vector<T>& Options, const T& Value) -> bool {
        return std::ranges::find(Options, Value) != Options.end();
    }

    template <CVarStorage T>
    [[nodiscard]] auto RegisterImpl(
        StringView Name, StringView Description, const T& DefaultValue, CVarTiming Timing, std::vector<T> Options)
        -> CVarEntry<T>& {
        auto&        Registry = Entries<T>();
        const String NameStr{Name};
        T            InitialValue = DefaultValue;
        if (!Options.empty() && !IsAllowedByOptions(Options, DefaultValue)) {
            InitialValue = Options.front();
            m_PendingDiagnostics.push_back(
                {.Message =
                     Format("CVar '{}' default is outside its declared options; using '{}'", NameStr, InitialValue),
                 .Level = LogLevel::Warning});
        }
        const auto [It, Inserted] =
            Registry.try_emplace(NameStr, String{Description}, InitialValue, Timing, std::move(Options));
        if (!Inserted) {
            m_PendingDiagnostics.push_back(
                {.Message = Format("Duplicate CVar registration '{}' ignored (first registration wins)", NameStr),
                 .Level   = LogLevel::Error});
        }
        return It->second;
    }

    template <CVarStorage T>
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
/// The primary template is selected through constrained partial specializations: storage types (bool,
/// Int32, Float32, String) use CVar<T>, enumerations use CVar<E>.  Register by constructing at namespace
/// scope in any translation unit: ~~~{.cpp}
///     CVar<bool> CVarRenderUseCSM{ "Render.UseCSM", true,
///         "Use cascaded shadow maps instead of a single shadow map" };
///
///     CVar<TestCVarMode> CVarRenderMode{ "Render.Mode", TestCVarMode::Safe, "render mode" };
///
///     if (CVarRenderUseCSM.Get()) { /* add CSM pass */ }
/// ~~~
///
/// Enumerations store their reflected enumerator names in the String registry, so Find<String> and
/// SetByName<String> interoperate with the same entry.  Enum CVars take no option list: every named
/// enumerator is a legal value, registered as the entry's options.  The enum itself is checked statically: it
/// must not be a flags enum and must not contain an "Unknown" guard value, and the default must be a named
/// enumerator.  Get and Set are safe to call from any thread.
export template <typename T>
class CVar;

/// @brief Storage-type CVar: self-registers with CVarManager and keeps a
///        zero-lookup handle to its registry entry.
export template <CVarStorage T>
class CVar<T> {
  public:
    /// @brief Construct and self-register with CVarManager.
    /// @param Name         Canonical dotted name, "Module.Feature" style,
    ///                     isomorphic to the TOML table path.
    /// @param DefaultValue The one-and-only default; the TOML config may
    ///                     override it at startup.
    /// @param Description  Human-readable explanation for future UI and logs.
    /// @param Timing       When readers consume the stored value.
    CVar(StringView Name, const T& DefaultValue, StringView Description, CVarTiming Timing = CVarTiming::Runtime)
        : m_Entry(CVarManager::Get().Register<T>(Name, Description, DefaultValue, {}, Timing)), m_Name(Name) {}

    /// @brief Construct and self-register with an ordered set of legal values.  An empty option list is a
    ///        contract violation: the registration is rejected with a queued error and the handle is inert
    ///        (Get yields the default, every write fails), rather than silently acting free-form.
    CVar(StringView         Name,
         const T&           DefaultValue,
         StringView         Description,
         std::span<const T> Options,
         CVarTiming         Timing = CVarTiming::Runtime)
        : m_Entry(Options.empty()
                      ? CVarManager::Get().RejectedEntry<T>(Name, "an option-bearing CVar requires at least one value")
                      : CVarManager::Get().Register<T>(Name, Description, DefaultValue, Options, Timing)),
          m_Name(Name) {}

    /// @brief Construct and self-register with a braced option list.  std::span cannot accept a braced
    ///        list directly, so brace call sites bind to this overload; container call sites bind to the
    ///        span overload above.
    CVar(StringView               Name,
         const T&                 DefaultValue,
         StringView               Description,
         std::initializer_list<T> Options,
         CVarTiming               Timing = CVarTiming::Runtime)
        : CVar(Name, DefaultValue, Description, std::span<const T>{Options.begin(), Options.size()}, Timing) {}

    /// @brief Read the current value.  Safe to call from any thread.
    [[nodiscard]] auto Get() const -> T {
        return *CVarManager::Get().FromStorageValue<T>(CVarManager::Get().LoadEntry(m_Entry));
    }

    /// @brief Write a new value.  Safe to call from any thread; readers
    ///        observe it on their next Get().  Values outside the declared
    ///        options are rejected with a warning.
    auto Set(const T& Value) -> void {
        CVarManager::Get().SetByName<T>(m_Name, Value);
    }

    /// @brief The canonical dotted name this CVar registered under.
    [[nodiscard]] auto Name() const -> StringView {
        return m_Name;
    }

  private:
    CVarEntry<T>& m_Entry; ///< Registry-owned entry; registration is infallible.
    StringView    m_Name;
};

/// @brief Enum-typed CVar: every named enumerator of E is a legal value and no option list is taken.
///        Registration stores all reflected names as the String entry's options, so writes through any path
///        (handle, SetByName<String>, TOML sweep) accept exactly the named enumerators.  See the CVar primary
///        template comment for the full enum contract.
export template <typename E>
    requires std::is_enum_v<E>
class CVar<E> {
    static_assert(!FlagEnum<E>, "CVar enums must not be flag enums");
    static_assert(std::ranges::none_of(magic_enum::enum_names<E>(),
                                       [](const std::string_view Name) -> bool { return Name == "Unknown"; }),
                  "CVar enums must not declare an \"Unknown\" guard value");

  public:
    /// @brief Construct and self-register with CVarManager.  The default must be a named enumerator and is
    ///        registered as the initial String value; the reflected names of all enumerators become the
    ///        entry's options.  An unnamed default is a contract violation: the registration is rejected
    ///        with a queued error and the handle is inert (Get yields the default, every write fails).
    CVar(StringView Name, E Default, StringView Description, CVarTiming Timing = CVarTiming::Runtime)
        : m_Default{Default}, m_Entry(MakeEntry(Name, Default, Description, Timing)), m_Name(Name) {}

    /// @brief Read the current typed value.  Safe to call from any thread;
    ///        falls back to the registration default when the stored name does
    ///        not parse.
    [[nodiscard]] auto Get() const -> E {
        return magic_enum::enum_cast<E>(CVarManager::Get().LoadEntry(m_Entry)).value_or(m_Default);
    }

    /// @brief Write a named enum value.  Safe to call from any thread; unnamed values are rejected with a
    ///        warning.
    auto Set(E Value) -> void {
        CVarManager::Get().SetByName<String>(m_Name, String{magic_enum::enum_name(Value)});
    }

    /// @brief The canonical dotted name this CVar registered under.
    [[nodiscard]] auto Name() const -> StringView {
        return m_Name;
    }

    /// @brief The registration default; also the fallback for unreadable stored values.
    [[nodiscard]] auto Default() const -> E {
        return m_Default;
    }

  private:
    /// @brief Register the String entry for one enum CVar.  An unnamed default is a contract violation
    ///        (magic_enum reflects no name for it), so the registration is rejected instead of storing an
    ///        empty string as the initial value.
    [[nodiscard]] static auto MakeEntry(StringView Name, E Default, StringView Description, CVarTiming Timing)
        -> CVarEntry<String>& {
        const auto DefaultName = magic_enum::enum_name(Default);
        if (DefaultName.empty())
            return CVarManager::Get().RejectedEntry<String>(Name, "the enum default must be a named enumerator");
        return CVarManager::Get().Register<String>(
            Name, Description, String{DefaultName}, EnumeratorNameList<E>(), Timing);
    }

    /// @brief Reflected names of every enumerator of E, in declaration order.
    template <typename U>
        requires std::is_enum_v<U>
    [[nodiscard]] static auto EnumeratorNameList() -> std::vector<String> {
        auto Names = magic_enum::enum_names<U>() |
                     std::views::transform([](const std::string_view Name) -> String { return String{Name}; });
        return {Names.begin(), Names.end()};
    }

    E                  m_Default;
    CVarEntry<String>& m_Entry; ///< Registry-owned entry; registration is infallible.
    StringView         m_Name;
};

/// @brief Deduction guides: storage types deduce the storage specialization, enumerations deduce the enum
///        specialization (which needs no option list).
template <CVarStorage T>
CVar(StringView, const T&, StringView, CVarTiming = CVarTiming::Runtime) -> CVar<T>;
template <CVarStorage T>
CVar(StringView, const T&, StringView, std::span<const T>, CVarTiming = CVarTiming::Runtime) -> CVar<T>;
template <typename E>
    requires std::is_enum_v<E>
CVar(StringView, E, StringView, CVarTiming = CVarTiming::Runtime) -> CVar<E>;

} // namespace SoulEngine
