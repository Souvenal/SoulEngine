/// @file   ShaderCompilerCache.cppm
/// @brief  In-memory shader compilation cache partition.

module;

#include <magic_enum/magic_enum.hpp>

export module ShaderCompiler:Cache;

export import :Types;

import Core;
import Shader;

export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::ShaderCompiler {

using CompileModuleFn = std::function<std::expected<std::vector<Shader::Program>, ErrorMessage>(const CompileDesc&)>;

struct CacheKey {
    Path    SourcePath = {};
    Backend Backend    = Backend::Unknown;
    String  EntryPoint = {};

    [[nodiscard]] auto operator==(const CacheKey&) const -> bool = default;
};

struct CacheKeyHash {
    [[nodiscard]] auto operator()(const CacheKey& Key) const -> std::size_t {
        std::size_t Hash  = std::hash<String>{}(Key.SourcePath.string());
        Hash             ^= std::hash<Uint8>{}(static_cast<Uint8>(Key.Backend)) << 1;
        Hash             ^= std::hash<String>{}(Key.EntryPoint) << 2;
        return Hash;
    }
};

/// @brief In-memory cache for compiled shader entry points.
class Cache : public Singleton<Cache> {
    friend class Singleton<Cache>;

  public:
    Cache(const Cache&)                    = delete;
    auto operator=(const Cache&) -> Cache& = delete;
    Cache(Cache&&)                         = delete;
    auto operator=(Cache&&) -> Cache&      = delete;

    /// @brief Return a compiled Shader::Program for a shader entry request.
    ///
    /// On cache miss, compiles the full shader module through CompileModule,
    /// caches every reflected entry point, and returns Entry.EntryPoint.
    [[nodiscard]] auto GetOrCompile(const ShaderEntry& Entry, const CompileModuleFn& CompileModule)
        -> std::expected<Shader::Program, ErrorMessage> {
        std::lock_guard CacheLock(m_CacheMutex);

        if (Entry.EntryPoint.empty()) {
            return std::unexpected(ErrorMessage("Shader cache: EntryPoint must not be empty"));
        }

        CacheKey LookupKey{
            .SourcePath = Entry.SourcePath.lexically_normal(),
            .Backend    = Entry.Backend,
            .EntryPoint = Entry.EntryPoint,
        };

        if (auto It = m_Cache.find(LookupKey); It != m_Cache.end()) {
            LogDebug("Shader cache hit '{}' / '{}'", LookupKey.SourcePath.string(), LookupKey.EntryPoint);
            return It->second;
        }

        LogDebug(
            "Shader cache miss '{}' / '{}' — compiling module", LookupKey.SourcePath.string(), LookupKey.EntryPoint);

        return CompileAndCacheAll(Entry, CompileModule);
    }

    /// @brief Clear all cached shader programs.
    auto Clear() -> void {
        std::lock_guard CacheLock(m_CacheMutex);
        m_Cache.clear();
    }

  private:
    Cache()  = default;
    ~Cache() = default;

    [[nodiscard]] auto CompileAndCacheAll(const ShaderEntry& Entry, const CompileModuleFn& CompileModule)
        -> std::expected<Shader::Program, ErrorMessage> {
        const auto&       Cfg = ConfigManager::Get();
        std::vector<Path> IncludeDirs{
            Cfg.EngineShadersDirPath(),
            Cfg.CurrentApplicationDir() / "Shaders",
        };

        auto Result = CompileModule(CompileDesc{
            .Source         = Entry.SourcePath,
            .Backend        = Entry.Backend,
            .EntryPointName = std::nullopt,
            .IncludeDirs    = IncludeDirs,
        });

        if (!Result) {
            return std::unexpected(Result.error().Append(
                Format("Shader cache: compile module failed for '{}'", Entry.SourcePath.string())));
        }

        for (const auto& Program : *Result) {
            CacheKey ProgramKey{
                .SourcePath = Entry.SourcePath.lexically_normal(),
                .Backend    = Entry.Backend,
                .EntryPoint = Program.EntryPointName,
            };
            LogDebug("Shader cache storing entry point '{}' (stage={}, file='{}')",
                     Program.EntryPointName,
                     magic_enum::enum_name(Program.Stage),
                     ProgramKey.SourcePath.string());
            m_Cache[ProgramKey] = Program;
        }

        CacheKey RequestedKey{
            .SourcePath = Entry.SourcePath.lexically_normal(),
            .Backend    = Entry.Backend,
            .EntryPoint = Entry.EntryPoint,
        };
        if (auto It = m_Cache.find(RequestedKey); It != m_Cache.end())
            return It->second;

        return std::unexpected(ErrorMessage(Format("Shader cache: entry point '{}' not found in compiled module '{}'",
                                                   Entry.EntryPoint,
                                                   Entry.SourcePath.string())));
    }

    std::mutex                                                  m_CacheMutex;
    std::unordered_map<CacheKey, Shader::Program, CacheKeyHash> m_Cache;
};

} // namespace SoulEngine::ShaderCompiler
