# Context: ShaderCache

**Namespace:** `SoulEngine::ShaderCache`

Shader compilation cache layer. The primary consumer of `ShaderCompiler` in normal code paths — callers request a compiled entry point via `GetOrCompile` and the cache either returns a hit or triggers a full-module compile and caches every entry point individually.

## Terms

| Term | Definition |
|------|------------|
| **ShaderCache** | Singleton facade exposing `GetOrCompile()`; owns an `unordered_map<ShaderCacheKey, Shader::Program>` |
| **ShaderCacheRequest** | Public request type — `SourcePath`, `EntryPoint`, `Backend`. Defines/IncludeDirs reserved as TODO. |
| **GetOrCompile** | The single public entry point. Given a request, returns the cached `Shader::Program` or compiles the full module, caches each entry point, and returns the requested one. |
| **CompileModule** | The internal strategy: forward `CompileDesc{nullopt entry}` to `ShaderCompiler::Compile()`, then iterate results and insert each `Program` into the cache with its own `ShaderCacheKey`. |

## Dependencies

- `ShaderCompiler` — for `Compile()`, which triggers Slang backend
- `Shader` — for `Shader::Program` (the cache value type)

## Relationships

- `ShaderCache` is the intended consumer of `ShaderCompiler` in production code paths. `ShaderCompiler::Compile()` can still be called directly for testing, hot-reload forced recompile, or bypass-cache scenarios.
- Cache lifetime: session-level (singleton, never cleared). `// TODO: cache eviction`.
- Thread safety: none yet (`// TODO: thread safety`); current usage is single-threaded.
- Not included yet: disk serialization, defines/include-dirs, cache eviction policy.
