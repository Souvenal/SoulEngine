# Context: Core

Foundation types, logging, config, and utilities shared by all other modules.

## Math

`Core:Util.Types` hosts the hlslpp integration glue shared by all modules:
EnTT `is_equality_comparable` specializations for hlslpp vector types,
required because hlslpp comparison operators return component-wise vector
results (HLSL semantics) that are a hard error inside EnTT's
equality-comparable probe.

## ECS

**SystemScheduler** (`Core:ECS`):
Owns and dispatches the ECS systems of a world. Every system registers under a
unique name with optional Before/After name lists declaring execution
dependencies; CompileDependency() resolves the dependency DAG (Kahn, ties
broken by registration order) into a deterministic execution order.
Registration and CompileDependency() are only allowed during the
initialization phase; OnUpdate() requires a successful CompileDependency().
Dangling name references and dependency cycles are reported as ErrorMessage
diagnostics (cycles include one concrete name path).
`Get<T>()` returns an optional borrowed reference wrapper; invoking it on a
const scheduler preserves a const system reference.
_Avoid_: relying on registration order for execution semantics, registering
systems mid-frame

## Language

### Type aliases

Type aliases in `Core:Util.Types` abstract away underlying implementations. Every module should use these aliases rather than raw standard types — swapping the backend (e.g. `std::string` → `eastl::string`) is then a single-file change.

**String / StringView**:
The project-wide string types (currently `std::string` / `std::string_view`). Used everywhere instead of `std::string` to decouple from the STL vendor.
_Avoid_: std::string, std::string_view, eastl::string (the last is a future option, not a current name)

**Int8 / Int16 / Int32 / Int64**:
Signed fixed-width integers (aliases for `std::int8_t` etc.).
**Uint8 / Uint16 / Uint32 / Uint64**:
Unsigned fixed-width integers (aliases for `std::uint8_t` etc.).

**Float32 / Float64**:
Fixed-width floating types (currently `float` / `double`; to be replaced by `std::float32_t` / `std::float64_t` when Clang supports `<stdfloat>`).

**UPtr\<T\> / SPtr\<T\>**:
Smart pointer aliases (`std::unique_ptr<T>` / `std::shared_ptr<T>`). The project always uses these aliases, never the raw standard names.
_Avoid_: std::unique_ptr, std::shared_ptr, std::make_unique, std::make_shared

**Path**:
Alias for `std::filesystem::path`.

**Format**:
A thin wrapper around `std::format` that avoids ambiguous overload resolution when both C++23 modules and preprocessor-based headers (Vulkan, GLFW, VMA) are mixed.

### Utilities

**ErrorMessage**:
A structured, human-readable error chain used as the error type for fallible engine APIs.
_Avoid_: Error string, status code, exception

**Singleton\<T\>**:
Meyer's singleton CRTP base class. Used by **LogManager**, **CVarManager**, and other module-level singletons. Provides a static `Get()` accessor and deletes all copy/move operations.
_Avoid_: Global instance, static instance

**ReadFile**:
Reads an entire file into a `String` (binary mode) using `ate` to pre-allocate exactly once. Returns `std::expected<String, ErrorMessage>` — success or error message.
_Avoid_: LoadFile, ReadWholeFile, slurp

### CVar system

**CVar\<T\>**:
A console variable: a namespace-scope object (`CVar<bool> CVarRenderUseCSM{"Render.UseCSM", true, "..."};`) that self-registers with CVarManager at static-init time and doubles as a zero-lookup access handle (`Get()`/`Set()`). Supported types: `bool`, `Int32`, `Float32`, `String`. Constructor argument order: name, default value, description. The default lives at the registration site and is the single source of truth.
_Avoid_: TAutoCVar, AutoCVar, central registration function

**CVarManager**:
The singleton registry behind all CVars. String access via `Find<T>(name) -> std::optional<T>` (name not registered under type T → `nullopt`, no logging) and `SetByName<T>(name, value) -> bool`. The registry key is (name, type): the same name may exist under several types. `LoadConfig(path)` flattens the TOML file's nested tables into dotted names and applies each leaf to the matching CVar; unknown keys and type mismatches are logged as warnings and skipped. Every CVar is runtime-mutable from any thread; readers poll, there are no change callbacks.
_Avoid_: CVarSystem ("System" clashes with ECS System vocabulary), ConsoleVariableManager

**Dotted CVar name**:
Canonical CVar identifier following the "Module.Feature" dot convention (e.g. `Render.UseCSM`, `RHI.Vulkan.Validation`), isomorphic to the TOML table nesting (`[RHI.Vulkan] Validation`).

**EnginePaths**:
A singleton answering "where is X" — engine root, Shaders/, Logs/, Configs/, Binaries/, Applications/. Paths are engine-environment facts, not configuration values.
_Avoid_: ConfigManager (retired: its config-value role moved to CVars, its path role moved here)

### Logging system

**LogManager**:
A singleton wrapping spdlog, dispatching log messages to sink(s) (file and/or console).
_Avoid_: Logger, LogSystem, LoggerManager

**LogLevel**:
An enum controlling verbosity: `Debug`, `Info`, `Warning`, `Error`.
_Avoid_: Verbosity, LogSeverity

**LogDebug / LogInfo / LogWarning / LogError**:
Global template functions that format and dispatch a log message at a given level through LogManager.

## Relationships

- **CVar\<T\>** registrations self-register into **CVarManager** at static-init time; **CVarManager** owns every CVar's storage and is the only TOML reader.
- **LogManager** dispatches at the granularity of **LogLevel**.
- **LogDebug / LogInfo / LogWarning / LogError** are convenience callers of **LogManager**.

## Example dialogue

> **Dev:** "I put `UseCSM = false` under `[Render]` in SoulEngine.toml, but the renderer still adds the CSM pass. Why?"
> **Domain expert:** "The TOML file is only a startup-time value source for registered **CVars** — it never creates them. If nobody registered a `Render.UseCSM` **CVar**, that key is warned about and ignored. Check the registration site; the default also lives there, not in the file."

## Flagged ambiguities

- "config" was used to mean both the I/O layer (ConfigManager) and individual subsystem settings — superseded: ConfigManager/EngineConfig were removed; runtime-mutable settings are **CVars**, and the TOML file is only their startup-time value source.
