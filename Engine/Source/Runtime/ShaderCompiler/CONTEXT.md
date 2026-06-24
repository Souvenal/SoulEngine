# Context: ShaderCompiler

**Namespace:** `SoulEngine::ShaderCompiler`

Shader compilation pipeline with a backend-per-language architecture.
The `ShaderCompiler` facade dispatches compile requests to internal
per-language backends. Backend selection is by source file extension.

Two compilation modes, controlled by `CompileDesc::EntryPointName`:
- **No value** — compile all entry points in the module into ShaderPrograms that share one bytecode blob
- **Has value** — compile only the named entry point into one ShaderProgram

## Terms

| Term | Definition |
|------|------------|
| **ShaderCompiler** | Public singleton facade. Entry point for cached and uncached shader compilation. Routes uncached compile requests to the correct backend. |
| **ShaderEntry** | Request for a single shader entry point in a source file. Used by the cache path. |
| **Cache** | In-memory `ShaderCompiler:Cache` partition used by `ShaderCompiler::GetOrCompile()`. |
| **CompileDesc** | Compile request descriptor — source path, optional inline source string, optional entry point name (`std::nullopt` = compile entire module), preprocessor defines, include directories. |
| **Program** | Shared Shader artifact produced by ShaderCompiler; one program represents one reflected entry point and its compiled bytecode. Refers to `Shader::Program`. |
| **IBackend** | Abstract interface for per-language compiler backends. Module-private. |
| **SlangCompiler** | Concrete Slang backend, in the `SlangCompiler::Backend` class. Translates `.slang` sources to SPIR-V via the Slang SDK. Implemented in the `SlangCompiler/` directory as module partitions `Slang:Types`, `Slang:Utils`, and `Slang:Reflection`, all within `SoulEngine::ShaderCompiler::SlangCompiler`. |
| **Backend** | Enum of supported backend languages. Currently only `Slang`. |

## Dependencies

- `Core` — logging, config
- `Shader` — `Shader::Stage`, `Shader::Program`
- Third-party: `vulkansdk` (Slang SDK headers: `slang.h`, `slang-com-ptr.h`)

## Relationships

- `CompileDesc::EntryPointName` is a compile-request selector; successful compile results take canonical entry-point name and stage from Slang reflection.
- A successful `Compile` produces at least one **Program**; Slang compilation failure and missing/invalid reflected entry points are distinct failure cases.
- Module-level compile preserves Slang reflection entry-point order in its returned `Program` values, but callers must select programs by reflected stage/name rather than relying on vector position.
- `ShaderCompiler::GetOrCompile()` is the production path. It uses the `ShaderCompiler:Cache` partition to compile whole modules, cache each reflected entry point, and return the requested `Shader::Program`.
- `ShaderCompiler::Compile()` remains public for tests, forced recompiles, and bypass-cache scenarios.
