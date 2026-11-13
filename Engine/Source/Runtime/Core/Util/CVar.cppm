module;
#include <toml++/toml.h>

export module Core.Util.CVar;

import Core.Util.Error;
import Core.Util.Flags;
import Core.Util.Logging;
import Core.Util.Singleton;
import Core.Util.Types;
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
///     A CVar<T> object constructed at namespace scope self-registers
///     with CVarManager during static initialization.  There is no central
///     registration function and no mirror config struct: the dotted name
///     (e.g. "Render.UseCSM") is the single canonical identifier.
///
///  2. **One registry, one namespace.**
///     All CVars — storage types and enums alike — live in a single
///     name-keyed map of type-erased entries; names are unique across all
///     value types.  The registering object is also the fast handle: its
///     constructor resolves its registry entry once; Get()/Set() then touch
///     the entry directly with zero lookup.  Code in other modules uses
///     CVarManager::Find<T>(name) / SetByName<T>(name, value), which return
///     std::optional / bool — a name not registered under type T yields
///     nullopt / false with no logging side effects.
///
///  3. **Registration failures are fatal at startup.**
///     Registration runs during single-threaded static init, where a check
///     can be decisive.  A duplicate name (across any types), an empty
///     option list on an option-bearing CVar, an enum default with no
///     reflected name, or a declared default outside its options are all
///     recorded as registration errors; LoadConfig() turns the first check
///     of that table into its error result, so Launch exits through its
///     normal startup-failure path before any frame runs.
///
///  4. **Defaults live at the registration site.**
///     The constructor's default value is the one-and-only default; the
///     TOML config file overrides it at startup via
///     CVarManager::LoadConfig().  Consumers never spell a fallback.
///     A storage CVar may declare ordered options; every write path
///     enforces them.  Enum CVars take no option list: every named
///     enumerator of the enum is legal, registered as the entry's options.
///
///  5. **Every CVar is runtime-mutable.**
///     Consumption timing declares when readers use a value: Runtime readers poll continuously; Startup
///     readers consume it during startup. Later writes update the stored value for either timing.
///     Arithmetic and enum values are atomic; String values sit behind a shared_mutex.
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

/// @brief Storage types backing the CVar registry: bool, Int32, Float32,
///        and String.
export template <typename T>
concept CVarStorage =
    std::same_as<T, bool> || std::same_as<T, Int32> || std::same_as<T, Float32> || std::same_as<T, String>;

/// @brief Value types a CVar can expose: storage types plus any enum.
///        Enum CVars own native entries; their values are stored and read
///        as the enum itself.
export template <typename T>
concept CVarValue = CVarStorage<T> || std::is_enum_v<T>;

/// @brief When a CVar's value is consumed.
export enum class CVarTiming : Uint8 {
    Unknown = 0,
    Runtime, ///< Readers poll the current value while running.
    Startup, ///< Readers consume the value during startup; later edits require restart.
};

/// @brief Monotonic type-id source for type-erased entry dispatch.  Called
///        once per instantiated CVarTypeId<T>, during single-threaded
///        static-init registration.
[[nodiscard]] inline auto NextCVarTypeId() -> Uint64 {
    static Uint64 Counter = 0;
    return ++Counter;
}

/// @brief Unique runtime id of one CVar value type.  Exported so UI code can
///        dispatch on entry types without per-type virtuals.
export template <CVarValue T>
[[nodiscard]] auto CVarTypeId() -> Uint64 {
    static const Uint64 Id = NextCVarTypeId();
    return Id;
}

/// @brief One registry entry: description, option metadata, and the value itself.
/// @details Entries live behind unique_ptr in the registry map, so their
///          addresses are stable for the process lifetime — that stability
///          is what lets CVar<T> handles cache an entry pointer at
///          registration and lets callers hold entry references across
///          frames.  Registration completes during single-threaded startup;
///          afterwards options and timing are immutable and only the value
///          is written concurrently, so a reader that reads one value at a
///          time needs no additional synchronization.  Arithmetic and enum
///          values are atomic; each String entry owns a shared_mutex
///          guarding its value.
template <CVarValue T>
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

/// @brief Convert a TOML leaf to a CVar value.  Integers undergo a range check for Int32; Float32 also
///        accepts TOML integers; enums parse their reflected name from a TOML string.
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
    } else if constexpr (std::is_enum_v<T>) {
        if (const auto V = Node.value<String>())
            return magic_enum::enum_cast<T>(*V);
        return std::nullopt;
    } else {
        return Node.value<String>();
    }
}

/// @brief Type-erased registry entry: dispatches config sweeps and the
///        settings panel without exposing the concrete value type.  The
///        panel's string-based accessors serve no public API — they exist
///        so one traversal can render every option-bearing CVar.
export class EntryBase {
  public:
    virtual ~EntryBase() = default;

    [[nodiscard]] virtual auto TypeId() const -> Uint64                                      = 0;
    [[nodiscard]] virtual auto Description() const noexcept -> StringView                    = 0;
    [[nodiscard]] virtual auto Timing() const noexcept -> CVarTiming                         = 0;
    /// @brief Convert a TOML leaf to the entry's value type and write it,
    ///        reporting a warning on conversion or option failure.
    virtual auto               TryApplyToml(StringView Name, const toml::node& Node) -> void = 0;
    /// @brief Ordered legal values as display strings; empty means free-form.
    [[nodiscard]] virtual auto OptionNames() const -> std::vector<String>                    = 0;
    [[nodiscard]] virtual auto LoadAsString() const -> String                                = 0;
    virtual auto               ApplyFromString(StringView Value) -> bool                     = 0;
};

/// @brief Concrete typed entry; owns the CVarEntry<T> a handle caches.
template <CVarValue T>
struct TypedEntry final : EntryBase {
    String       Name;
    CVarEntry<T> Entry;

    TypedEntry(
        String NameValue, String DescriptionValue, T InitialValue, CVarTiming EntryTiming, std::vector<T> EntryOptions)
        : Name{std::move(NameValue)},
          Entry{std::move(DescriptionValue), InitialValue, EntryTiming, std::move(EntryOptions)} {}

    [[nodiscard]] auto TypeId() const -> Uint64 override {
        return CVarTypeId<T>();
    }
    [[nodiscard]] auto Description() const noexcept -> StringView override {
        return Entry.Description;
    }
    [[nodiscard]] auto Timing() const noexcept -> CVarTiming override {
        return Entry.Timing;
    }
    auto               TryApplyToml(StringView Name, const toml::node& Node) -> void override;
    [[nodiscard]] auto OptionNames() const -> std::vector<String> override;
    [[nodiscard]] auto LoadAsString() const -> String override;
    auto               ApplyFromString(StringView Value) -> bool override;
};

/// @brief Recover the typed entry behind a type-erased one.
template <CVarValue T>
[[nodiscard]] auto TypedAs(EntryBase& Entry) -> CVarEntry<T>* {
    if (Entry.TypeId() != CVarTypeId<T>())
        return nullptr;
    return &static_cast<TypedEntry<T>&>(Entry).Entry;
}

/// @brief Central registry of all console variables.
///
/// Registration happens implicitly at static-init time via CVar<T> constructors. Startup then applies the
/// TOML config through LoadConfig(): the file's nested tables are flattened into dotted names ("[RHI.Vulkan]
/// Validation" becomes "RHI.Vulkan.Validation") and each leaf value is written to the matching CVar.
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
        const auto It = m_Entries.find(String{Name});
        if (It == m_Entries.end())
            return std::nullopt;
        const auto* Entry = TypedAs<T>(*It->second);
        if (Entry == nullptr)
            return std::nullopt;
        return LoadEntry(*Entry);
    }

    /// @brief Visit every registered CVar, type-erased.  Used by the
    ///        settings panel to discover bool and option-bearing entries.
    auto ForEachEntry(auto&& Visitor) -> void {
        for (const auto& [Name, Entry] : m_Entries)
            Visitor(StringView{Name}, *Entry);
    }

    /// @brief Write a CVar's value by name.
    /// @return false when the name is not registered under type T or the
    ///         value is outside the declared options.
    template <CVarValue T>
    auto SetByName(StringView Name, const T& Value) -> bool {
        const auto It = m_Entries.find(String{Name});
        if (It == m_Entries.end())
            return false;
        auto* Entry = TypedAs<T>(*It->second);
        if (Entry == nullptr)
            return false;
        if (!StoreEntry(*Entry, Value)) {
            LogWarning("CVar '{}' value is outside its declared options; value ignored", Name);
            return false;
        }
        return true;
    }

    /// @brief Register a CVar and return its registry entry.  An empty Options span registers a free-form
    ///        CVar that accepts every value; a non-empty span registers an ordered set of legal values, and
    ///        a default outside them is fatal.  Enum CVars take no option list: every named enumerator is
    ///        registered as legal, and an unnamed default is fatal.
    /// @return The registry entry (stable for the process lifetime), or
    ///         nullptr when the registration was rejected as fatal; the
    ///         error is recorded for LoadConfig() to surface, so Launch
    ///         exits through its normal startup-failure path.
    template <CVarValue T>
    auto Register(StringView         Name,
                  StringView         Description,
                  const T&           DefaultValue,
                  std::span<const T> Options = {},
                  CVarTiming         Timing  = CVarTiming::Runtime) -> CVarEntry<T>* {
        const String NameStr{Name};
        if (m_Entries.contains(NameStr)) {
            RecordRegistrationError(Name, "duplicate name (the first registration owns it)");
            return nullptr;
        }
        std::vector<T> EntryOptions = {};
        if constexpr (std::is_enum_v<T>) {
            const auto Values = magic_enum::enum_values<T>();
            EntryOptions.assign(Values.begin(), Values.end());
            if (magic_enum::enum_name(DefaultValue).empty()) {
                RecordRegistrationError(Name, "the enum default must be a named enumerator");
                return nullptr;
            }
        } else {
            EntryOptions.assign(Options.begin(), Options.end());
            if (!EntryOptions.empty() && std::ranges::find(EntryOptions, DefaultValue) == EntryOptions.end()) {
                RecordRegistrationError(Name, "the default must be one of the declared options");
                return nullptr;
            }
        }
        auto Entry = std::make_unique<TypedEntry<T>>(
            NameStr, String{Description}, DefaultValue, Timing, std::move(EntryOptions));
        const auto [It, Inserted] = m_Entries.try_emplace(String{Name}, std::move(Entry));
        static_cast<void>(Inserted);
        return &static_cast<TypedEntry<T>&>(*It->second).Entry;
    }

    /// @brief Record a fatal registration error on behalf of a handle whose
    ///        rejection the manager itself could not classify.  Surfaced
    ///        through LoadConfig()'s error result.
    auto RecordRegistrationError(StringView Name, StringView Reason) -> void {
        m_RegistrationErrors.push_back(Format("CVar '{}': {}", Name, Reason));
    }

    /// @brief Registration errors recorded during static-init registration.
    [[nodiscard]] auto RegistrationErrors() const -> const std::vector<String>& {
        return m_RegistrationErrors;
    }

    /// @brief Discard recorded registration errors.  Intended for test hosts
    ///        that deliberately register rejected CVars and then exercise
    ///        LoadConfig(); Launch never calls this — a recorded error is
    ///        fatal there.
    auto ClearRegistrationErrors() -> void {
        m_RegistrationErrors.clear();
    }

    /// @brief Parse a TOML config file and apply every leaf value to the
    ///        CVar whose dotted name matches its table path.  Registration
    ///        errors recorded during static init are surfaced here, the
    ///        first point where Launch's normal error path can act on them.
    /// @retval std::unexpected Registration errors or a parse error (e.g.
    ///         file not found or invalid TOML).
    [[nodiscard]] auto LoadConfig(const Path& FilePath) -> std::expected<void, ErrorMessage>;

    template <typename T>
    friend class CVar;

  private:
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
        if (!Entry.Options.empty() && std::ranges::find(Entry.Options, Value) == Entry.Options.end())
            return false;
        if constexpr (std::same_as<T, String>) {
            std::unique_lock Lock{Entry.Mutex};
            Entry.Value = Value;
        } else {
            Entry.Value.store(Value, std::memory_order_relaxed);
        }
        return true;
    }

    auto ApplyTable(const toml::table& Table, const String& Prefix) -> void;

    auto ApplyValue(const String& Name, const toml::node& Node) -> void;

    std::unordered_map<String, std::unique_ptr<EntryBase>> m_Entries;
    std::vector<String>                                    m_RegistrationErrors;
};

template <CVarValue T>
auto TypedEntry<T>::TryApplyToml(StringView Name, const toml::node& Node) -> void {
    const auto Converted = ConvertTomlNode<T>(Node);
    if (!Converted) {
        LogWarning("Config key '{}' value does not match the CVar's type; value ignored", Name);
        return;
    }
    static_cast<void>(CVarManager::Get().SetByName<T>(Name, *Converted));
}

template <CVarValue T>
auto TypedEntry<T>::OptionNames() const -> std::vector<String> {
    std::vector<String> Names = {};
    if constexpr (std::same_as<T, String>) {
        Names = Entry.Options;
    } else if constexpr (std::is_enum_v<T>) {
        for (const auto& Value : Entry.Options)
            Names.emplace_back(magic_enum::enum_name(Value));
    }
    return Names;
}

template <CVarValue T>
auto TypedEntry<T>::LoadAsString() const -> String {
    if constexpr (std::same_as<T, String>) {
        return CVarManager::Get().Find<String>(Name).value_or(String{});
    } else if constexpr (std::is_enum_v<T>) {
        const auto Value = CVarManager::Get().Find<T>(Name);
        return Value ? String{magic_enum::enum_name(*Value)} : String{};
    } else {
        return {};
    }
}

template <CVarValue T>
auto TypedEntry<T>::ApplyFromString(StringView Value) -> bool {
    if constexpr (std::same_as<T, String>) {
        return CVarManager::Get().SetByName<T>(Name, String{Value});
    } else if constexpr (std::is_enum_v<T>) {
        const auto Parsed = magic_enum::enum_cast<T>(Value);
        return Parsed && CVarManager::Get().SetByName<T>(Name, *Parsed);
    } else {
        return false;
    }
}

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
/// Enumerations own native entries: their values are stored and read as the enum itself, and no option
/// list is taken — every named enumerator is a legal value, registered as the entry's options.  The enum
/// itself is checked statically: it must not be a flags enum and must not contain an "Unknown" guard value.
/// A handle whose registration was rejected serves its declared default and rejects every write.  Get and
/// Set are safe to call from any thread.
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
        : m_Default{DefaultValue},
          m_Entry(CVarManager::Get().Register<T>(Name, Description, DefaultValue, {}, Timing)),
          m_Name(Name) {}

    /// @brief Construct and self-register with an ordered set of legal values.  An empty option list is a
    ///        contract violation: the registration is recorded as fatal and the handle falls back to its
    ///        declared default (Launch exits before any read can matter), rather than silently acting
    ///        free-form.
    CVar(StringView         Name,
         const T&           DefaultValue,
         StringView         Description,
         std::span<const T> Options,
         CVarTiming         Timing = CVarTiming::Runtime)
        : m_Default{DefaultValue},
          m_Entry(Options.empty() ? RejectedOptionsEntry(Name)
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

    /// @brief Read the current value.  Safe to call from any thread.  A
    ///        rejected registration yields the declared default.
    [[nodiscard]] auto Get() const -> T {
        return m_Entry ? CVarManager::Get().LoadEntry(*m_Entry) : m_Default;
    }

    /// @brief Write a new value.  Safe to call from any thread; readers
    ///        observe it on their next Get().  Values outside the declared
    ///        options are rejected with a warning.
    auto Set(const T& Value) -> void {
        if (m_Entry)
            CVarManager::Get().SetByName<T>(m_Name, Value);
    }

    /// @brief The canonical dotted name this CVar registered under.
    [[nodiscard]] auto Name() const -> StringView {
        return m_Name;
    }

  private:
    [[nodiscard]] static auto RejectedOptionsEntry(StringView Name) -> CVarEntry<T>* {
        CVarManager::Get().RecordRegistrationError(Name, "an option-bearing CVar requires at least one value");
        return nullptr;
    }

    T             m_Default; ///< Fallback for a rejected registration.
    CVarEntry<T>* m_Entry;   ///< Registry-owned entry; null after a rejected registration.
    StringView    m_Name;
};

/// @brief Enum-typed CVar: every named enumerator of E is a legal value and no option list is taken.
///        Registration stores all reflected enumerators as the entry's options, so writes through any path
///        accept exactly the named enumerators.  See the CVar primary template comment for the full enum
///        contract.
export template <typename E>
    requires std::is_enum_v<E>
class CVar<E> {
    static_assert(!FlagEnum<E>, "CVar enums must not be flag enums");
    static_assert(std::ranges::none_of(magic_enum::enum_names<E>(),
                                       [](const std::string_view Name) -> bool { return Name == "Unknown"; }),
                  "CVar enums must not declare an \"Unknown\" guard value");

  public:
    /// @brief Construct and self-register with CVarManager.  The default must be a named enumerator and is
    ///        registered as the initial value; every reflected enumerator becomes a legal value.
    CVar(StringView Name, E Default, StringView Description, CVarTiming Timing = CVarTiming::Runtime)
        : m_Default{Default},
          m_Entry(CVarManager::Get().Register<E>(Name, Description, Default, {}, Timing)),
          m_Name(Name) {}

    /// @brief Read the current typed value.  Safe to call from any thread.  A rejected registration
    ///        yields the declared default.
    [[nodiscard]] auto Get() const -> E {
        return m_Entry ? CVarManager::Get().LoadEntry(*m_Entry) : m_Default;
    }

    /// @brief Write a named enum value.  Safe to call from any thread; unnamed values are rejected with a
    ///        warning.
    auto Set(E Value) -> void {
        if (m_Entry)
            CVarManager::Get().SetByName<E>(m_Name, Value);
    }

    /// @brief The canonical dotted name this CVar registered under.
    [[nodiscard]] auto Name() const -> StringView {
        return m_Name;
    }

  private:
    E             m_Default; ///< Fallback for a rejected registration or an unreadable stored value.
    CVarEntry<E>* m_Entry;   ///< Registry-owned entry; null after a rejected registration.
    StringView    m_Name;
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
