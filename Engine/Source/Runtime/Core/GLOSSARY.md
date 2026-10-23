# Core

Foundation module: types, logging, config, and utilities shared by every other module.

## Language

### ECS

**SystemScheduler**:
Owns and dispatches a world's ECS systems. Systems register by unique name with Before/After dependency declarations; the resolved execution order is deterministic, and registration closes when initialization ends.
_Avoid_: relying on registration order for execution semantics, mid-frame registration

### Assets

**AssetManager**:
Process-wide asset registry. Loading an asset registers its path and returns a stable handle; content bytes are read lazily and cached read-only for the process lifetime.
_Avoid_: treating Load as a file read, storing asset paths in components (persist the handle)

**AssetHandle**:
Stable opaque identifier for a registered asset path; id 0 is the invalid sentinel.

### Language

**Type aliases**:
Project-wide aliases (String, StringView, the fixed-width Int/Uint/Float family, UPtr, SPtr, Path) abstract the underlying standard types so a vendor swap is a single-file change. Every module uses the aliases, never the raw standard names.
_Avoid_: std::string, std::string_view, std::unique_ptr, std::shared_ptr, std::make_unique, std::make_shared

**Flag enums**:
An enum opts into flag composition by declaring itself a flags enum; every module then composes its flags via ADL with no per-file using and no hand-written operators.
_Avoid_: per-enum hand-written bit operators, per-file using of the flag operator, wholesale export of magic_enum bitwise operators

**ErrorMessage**:
Structured, human-readable error chain; the error type of every fallible engine API.
_Avoid_: error string, status code, exception

**Singleton**:
Meyer's-singleton CRTP base for module-level singletons; provides the static accessor and deletes copy/move.
_Avoid_: global instance, static instance

### CVar system

**CVar**:
Console variable — a namespace-scope object that self-registers at static-init time and doubles as a zero-lookup access handle. The default lives at the registration site and is the single source of truth.
_Avoid_: TAutoCVar, AutoCVar, central registration function

**CVarManager**:
Singleton registry behind all CVars and the only reader of the TOML config file: the file's nested tables flatten into dotted names applied to matching CVars, and unknown keys are warned about and skipped. CVars are runtime-mutable from any thread; readers poll — there are no change callbacks.
_Avoid_: CVarSystem, ConsoleVariableManager

**Dotted CVar name**:
Canonical CVar identifier in "Module.Feature" dot convention, isomorphic to the TOML table nesting.

**EnginePaths**:
Singleton answering "where is X" (engine root, Shaders/, Logs/, Configs/, Binaries/, Applications/). Paths are engine-environment facts, not configuration values.
_Avoid_: ConfigManager (retired)

### Reflection

**FieldDescriptor**:
Type-erased accessor pair (name, FieldType, Get, Set) for one reflected field; registration sites build descriptors from compile-time member pointers, so the table is the only hand-maintained surface.
_Avoid_: runtime type registration, RTTI-based reflection

**FieldTable**:
The full descriptor set of one reflected type, declared as a static constexpr member of the type itself. YAML loading and editor panels consume the same table; presentation metadata lives in a separate editor-side table.
_Avoid_: per-consumer registration, reflection macros

### Logging

**LogManager**:
Singleton wrapping spdlog, dispatching messages to file and/or console sinks.
_Avoid_: Logger, LogSystem, LoggerManager

**LogLevel**:
Verbosity enum: Debug, Info, Warning, Error.
_Avoid_: Verbosity, LogSeverity
