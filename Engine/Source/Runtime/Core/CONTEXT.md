# Context: Core

**Namespace:** `SoulEngine::Core`

Foundation types, logging, config, and utilities shared by all other modules.

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
Meyer's singleton CRTP base class. Used by **LogManager**, **ConfigManager**, and other module-level singletons. Provides a static `Get()` accessor and deletes all copy/move operations.
_Avoid_: Global instance, static instance

**ReadFile**:
Reads an entire file into a `String` (binary mode) using `ate` to pre-allocate exactly once. Returns `std::expected<String, ErrorMessage>` — success or error message.
_Avoid_: LoadFile, ReadWholeFile, slurp

### Config system

**ConfigManager**:
A singleton that parses TOML config files and populates an `EngineConfig` struct.
_Avoid_: ConfigParser, SettingsManager, ConfigLoader

**EngineConfig**:
The root config struct aggregating all subsystem configurations. Each subsystem (Window, Render, Log, Application, Shader) has its own nested struct mirroring the corresponding TOML table.
_Avoid_: GlobalConfig, AppConfig

**Optional-field config design**:
All config struct fields are `std::optional`. ConfigManager is a pure I/O layer — it fills what was present in TOML and leaves absent keys as `nullopt`. Each consuming module owns its own default via `value_or(...)`, avoiding a single point of truth drift.
_Avoid_: Defaults in config layer, non-optional config fields

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

- **ConfigManager** uses **EngineConfig** as its internal data store; it is the only writer to it.
- **LogManager** dispatches at the granularity of **LogLevel**.
- **LogDebug / LogInfo / LogWarning / LogError** are convenience callers of **LogManager**.

## Example dialogue

> **Dev:** "When I load `SoulEngine.example.toml`, the `Render.FramesInFlight` field is `nullopt` — won't the engine crash?"
> **Domain expert:** "No — ConfigManager is a pure I/O layer. It doesn't know what a reasonable value is. **RenderConfig** just mirrors what was in the TOML file. The RHI module reads `Cfg.Render.FramesInFlight.value_or(2)` and applies its own fallback."

## Flagged ambiguities

- "config" was used to mean both the I/O layer (ConfigManager) and individual subsystem settings — resolved: the former is a type, the latter are nested structs under EngineConfig.
