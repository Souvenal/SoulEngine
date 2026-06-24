export module ShaderCache;

import Core;
import Shader;
export import ShaderCompiler;

export import std;

using namespace SoulEngine::Core;

namespace SoulEngine::ShaderCache {

/// @brief Public request type for shader compilation.
export struct ShaderCacheRequest {
    ShaderCompiler::Backend Backend = ShaderCompiler::Backend::Slang;
    Path                    SourcePath;
    String                  EntryPoint;

    // TODO: 后续需要按 entry point 生成变体时启用
    // std::span<const StringView> Defines;
    // std::span<const Path> IncludeDirs;
};

// ── Internal cache types (not exported) ──────────────────────────────

struct Key {
    Path                    SourcePath;
    ShaderCompiler::Backend Backend;
    String                  EntryPoint;

    // TODO: Defines, IncludeDirs — add to hash and equality when enabled

    [[nodiscard]] auto operator==(const Key&) const -> bool = default;
};

struct KeyHash {
    [[nodiscard]] auto operator()(const Key& K) const -> std::size_t {
        std::size_t H  = std::hash<String>{}(K.SourcePath.string());
        H             ^= std::hash<Uint8>{}(static_cast<Uint8>(K.Backend)) << 1;
        H             ^= std::hash<String>{}(K.EntryPoint) << 2;
        return H;
    }
};

/// Global in-memory cache: key -> compiled Program.
/// Module-global because ShaderCache is a singleton — there is only one
/// cache for the lifetime of the process.
// NOLINT(cert-err58-cpp) — intentional global, init order is fine for an empty map
std::unordered_map<Key, Shader::Program, KeyHash> g_Cache;
std::mutex                                        g_CacheMutex;

// TODO: cache eviction — LRU or never-expire policy

/// Compile the full module and cache every entry point.
[[nodiscard]] auto CompileAndCacheAll(const ShaderCacheRequest& Req) -> std::expected<Shader::Program, ErrorMessage> {
    // Build default include dirs: engine shaders + current app shaders.
    const auto&       Cfg = ConfigManager::Get();
    std::vector<Path> IncludeDirs{
        Cfg.EngineShadersDirPath(),
        Cfg.CurrentApplicationDir() / "Shaders",
    };

    auto Result = ShaderCompiler::ShaderCompiler::Get().Compile(ShaderCompiler::CompileDesc{
        .Source         = Req.SourcePath,
        .Backend        = Req.Backend,
        .EntryPointName = std::nullopt,
        .IncludeDirs    = IncludeDirs,
    });

    if (!Result) {
        return std::unexpected(
            Result.error().Append(Format("ShaderCache: CompileModule failed for '{}'", Req.SourcePath.string())));
    }

    // Cache every entry point from the compiled module.
    for (const auto& Program : *Result) {
        Key CacheKey{
            .SourcePath = Req.SourcePath.lexically_normal(),
            .Backend    = Req.Backend,
            .EntryPoint = Program.EntryPointName,
        };
        LogDebug("ShaderCache: caching entry point '{}' (stage={}, file='{}')",
                 Program.EntryPointName,
                 magic_enum::enum_name(Program.Stage),
                 CacheKey.SourcePath.string());
        g_Cache[CacheKey] = Program;
    }

    // Now find the requested entry point in the cache.
    Key ReqKey{
        .SourcePath = Req.SourcePath.lexically_normal(),
        .Backend    = Req.Backend,
        .EntryPoint = Req.EntryPoint,
    };
    if (auto It = g_Cache.find(ReqKey); It != g_Cache.end())
        return It->second;

    return std::unexpected(ErrorMessage(Format(
        "ShaderCache: entry point '{}' not found in compiled module '{}'", Req.EntryPoint, Req.SourcePath.string())));
}

/// @brief In-memory shader compilation cache.
///
/// Singleton.  Primary public entry point for obtaining compiled shader
/// programs.  On cache miss, the entire module is compiled (all entry
/// points) and each entry point is cached individually — subsequent
/// requests for other entry points in the same file hit immediately.
export class ShaderCache : public Singleton<ShaderCache> {
    friend class Singleton<ShaderCache>;

  public:
    ShaderCache(const ShaderCache&)                    = delete;
    auto operator=(const ShaderCache&) -> ShaderCache& = delete;
    ShaderCache(ShaderCache&&)                         = delete;
    auto operator=(ShaderCache&&) -> ShaderCache&      = delete;

    /// @brief Return a compiled Shader::Program for the given request.
    ///
    /// First checks the in-memory cache.  On miss, compiles the full
    /// shader module, caches every entry point, and returns the one
    /// matching Req.EntryPoint.
    [[nodiscard]] auto GetOrCompile(const ShaderCacheRequest& Req) -> std::expected<Shader::Program, ErrorMessage> {
        std::lock_guard CacheLock(g_CacheMutex);

        if (Req.EntryPoint.empty()) {
            return std::unexpected(ErrorMessage("ShaderCache: EntryPoint must not be empty"));
        }

        Key LookupKey{
            .SourcePath = Req.SourcePath.lexically_normal(),
            .Backend    = Req.Backend,
            .EntryPoint = Req.EntryPoint,
        };

        if (auto It = g_Cache.find(LookupKey); It != g_Cache.end()) {
            LogDebug("ShaderCache: hit '{}' / '{}'", LookupKey.SourcePath.string(), LookupKey.EntryPoint);
            return It->second;
        }

        LogDebug(
            "ShaderCache: miss '{}' / '{}' — compiling module", LookupKey.SourcePath.string(), LookupKey.EntryPoint);

        return CompileAndCacheAll(Req);
    }

  private:
    ShaderCache()  = default;
    ~ShaderCache() = default;
};

} // namespace SoulEngine::ShaderCache
