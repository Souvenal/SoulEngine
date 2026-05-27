module;

#include <toml++/toml.h>

export module Core:Config;

export import :Util;
import :Logging;

export import std;

/// @file
/// @brief Engine configuration loading (TOML → C++ structs).
///
/// @section config_design  Design Decision — All fields are std::optional
///
/// Every config struct field is `std::optional` rather than carrying a
/// hard-coded default value.  The rationale:
///
///  1. **Single source of truth for defaults.**
///     Each module that *consumes* a config value owns its own fallback
///     (via `.value_or(...)`).  If two places both spell out a default they
///     will inevitably drift — here the config layer simply has no defaults
///     to drift.
///
///  2. **Clear separation of concerns.**
///     ConfigManager is a pure I/O layer: parse TOML, fill what was present,
///     leave everything else as `nullopt`.  It knows nothing about what values
///     are "reasonable" — that is a policy decision for the consumer.
///
///  3. **Absence is distinguishable from an explicit value.**
///     With non-optional fields and `value_or(field)`, a TOML key that was
///     absent produced the same result as one explicitly set to the default.
///     Optional fields preserve this information for consumers that need it.
///
///  4. **LoadFile is a blind copy.**
///     Each line of `LoadFile` is a straight `Cfg.X = table[...].value<T>()`
///     with no fallback logic — easy to audit and impossible to accidentally
///     use the wrong default.
///
/// Consumers call `cfg.Field.value_or(fallback)` where the fallback is the
/// one-and-only default for that module.

export namespace SoulEngine::Core {
/// @brief Window subsystem configuration.
///
/// Pure mirror of the `[Window]` TOML table.  All fields are optional;
/// the Window module provides the actual defaults.
struct WindowConfig {
    std::optional<Int32> ResolutionX; ///< Window width
    std::optional<Int32> ResolutionY; ///< Window height
};

/// @brief Render subsystem configuration.
///
/// Pure mirror of the `[Render]` TOML table.  All fields are optional;
/// RHI backends provide the actual defaults.
struct RenderConfig {
    std::optional<String> RHI;            ///< Backend name
    std::optional<Int32>  FramesInFlight; ///< Number of frames in flight
};

/// @brief Log-level configuration.
///
/// Pure mirror of the `[Log]` TOML table.  All fields are optional;
/// LogManager provides the actual defaults at the sink level.
struct LogConfig {
    std::optional<String> ConsoleLevel; ///< Min level for console output
    std::optional<String> FileLevel;    ///< Min level for file output
};

/// @brief Application-level configuration.
///
/// Pure mirror of the `[Application]` TOML table.
struct ApplicationConfig {
    std::optional<String> Name; ///< Application name
};

/// @brief Shader compiler configuration.
///
/// Pure mirror of the `[Shader]` TOML table.  All fields are optional;
/// the ShaderCompiler module provides the actual defaults.
struct ShaderConfig {
    std::optional<bool> DebugInfo; ///< Emit SPIR-V debug info for RenderDoc
};

/// @brief Vulkan RHI backend configuration.
///
/// Pure mirror of the `[RHI.Vulkan]` TOML table.  All fields are optional;
/// the Vulkan backend provides the actual defaults.
struct VulkanConfig {
    std::optional<Uint32> MaxTextures; ///< Bindless texture descriptor slots
};

/// @brief Top-level engine configuration, aggregating all subsystems.
///
/// This struct is the root of the config hierarchy.  It holds no
/// logic — only data.  Populated by ConfigManager::LoadFile.
struct EngineConfig {
    WindowConfig      Window;      ///< Window settings    (from [Window])
    RenderConfig      Render;      ///< Render settings    (from [Render])
    LogConfig         Log;         ///< Log settings       (from [Log])
    ApplicationConfig Application; ///< Application info   (from [Application])
    ShaderConfig      Shader;      ///< Shader compiler    (from [Shader])
    VulkanConfig      RhiVulkan;   ///< Vulkan RHI         (from [RHI.Vulkan])
};

/// @brief Singleton that loads and exposes engine configuration.
///
/// Usage:
/// ~~~{.cpp}
///     auto& Mgr = ConfigManager::Get();
///     Mgr.Init("/path/to/engine");
///     if (auto R = Mgr.LoadConfig(); !R)
///         return R;
///
///     const auto& Cfg = Mgr.GetConfig();
///     int ResX = Cfg.Window.ResolutionX.value_or(1280);
/// ~~~
///
/// For advanced use, LoadFile(const Path&) is still available to load
/// a config from an arbitrary path outside the engine directory tree.
///
/// @note This class is intentionally thin — it parses TOML and stores
///       the result.  Default values are owned by the consuming modules,
///       not here.  See @ref config_design for the full rationale.
class ConfigManager final : public Singleton<ConfigManager> {
    friend class Singleton<ConfigManager>;
    ConfigManager() = default;

  public:
    /// @brief Initialize with the engine root directory.
    /// @details Stores the engine root and pre-computes well-known
    ///          sub-directory paths (Shaders, Logs).  No I/O — call
    ///          LoadConfig() separately to parse the TOML config.
    /// @param EngineDirPath Absolute path to the engine root (the
    ///                      directory containing Configs/, Shaders/,
    ///                      Logs/, etc.).
    auto Init(const Path& EngineDirPath) -> void {
        m_EngineDirPath   = EngineDirPath;
        m_ShadersDirPath  = EngineDirPath / "Shaders";
        m_LogsDirPath     = EngineDirPath / "Logs";
        m_ConfigsDirPath  = EngineDirPath / "Configs";
        m_BinariesDirPath = EngineDirPath / "Binaries";
    }

    /// @brief Parse the config file at EngineDir/Configs/SoulEngine.toml.
    /// @retval std::unexpected  Parse error.
    /// @pre Init() must have been called first.
    [[nodiscard]] auto LoadConfig() -> std::expected<void, ErrorMessage> {
        Path CfgPath = m_ConfigsDirPath / "SoulEngine.toml";
        return LoadFile(CfgPath);
    }

    /// @brief Access the engine root directory.
    [[nodiscard]] auto EngineDirPath() const -> const Path& {
        return m_EngineDirPath;
    }

    /// @brief Path to Engine/Shaders/.
    [[nodiscard]] auto ShadersDirPath() const -> const Path& {
        return m_ShadersDirPath;
    }

    /// @brief Path to Engine/Logs/.
    [[nodiscard]] auto LogsDirPath() const -> const Path& {
        return m_LogsDirPath;
    }

    /// @brief Path to Engine/Configs/.
    [[nodiscard]] auto ConfigsDirPath() const -> const Path& {
        return m_ConfigsDirPath;
    }

    /// @brief Path to Engine/Binaries/.
    [[nodiscard]] auto BinariesDirPath() const -> const Path& {
        return m_BinariesDirPath;
    }

    /// @brief Parse a TOML file and populate the config struct.
    /// @param FilePath Path to the .toml file (absolute or relative to CWD).
    /// @retval std::unexpected  Parse error (e.g. file not found or invalid TOML).
    [[nodiscard]] auto LoadFile(const Path& FilePath) -> std::expected<void, ErrorMessage> {
        auto Result = toml::parse_file(FilePath.string());
        if (!Result) {
            auto& Err = Result.error();
            return std::unexpected(ErrorMessage(Format("Failed to parse '{}':\n  {} (at line {}, column {})",
                                                       FilePath.string(),
                                                       Err.description(),
                                                       Err.source().begin.line,
                                                       Err.source().begin.column)));
        }

        auto Table = std::move(Result).table();
        LogInfo("Loaded config file: {}", FilePath.string());

        // All Config fields are std::optional — missing TOML keys leave
        // nullopt, consumers provide their own .value_or(...) defaults.

        // --- Window config ---
        Cfg.Window.ResolutionX = Table["Window"]["ResolutionX"].value<Int32>();
        Cfg.Window.ResolutionY = Table["Window"]["ResolutionY"].value<Int32>();

        // --- Render config ---
        Cfg.Render.RHI            = Table["Render"]["RHI"].value<String>();
        Cfg.Render.FramesInFlight = Table["Render"]["FramesInFlight"].value<Int32>();

        // --- Log config ---
        Cfg.Log.ConsoleLevel = Table["Log"]["ConsoleLevel"].value<String>();
        Cfg.Log.FileLevel    = Table["Log"]["FileLevel"].value<String>();

        // --- Application config ---
        Cfg.Application.Name = Table["Application"]["Name"].value<String>();

        // --- Shader config ---
        Cfg.Shader.DebugInfo = Table["Shader"]["DebugInfo"].value<bool>();

        // --- Vulkan RHI config ---
        Cfg.RhiVulkan.MaxTextures         = Table["RHI"]["Vulkan"]["MaxTextures"].value<Uint32>();

        return {};
    }

    /// @brief Access the loaded configuration (read-only).
    /// @return Const reference to the internal EngineConfig.
    [[nodiscard]] auto GetConfig() const -> const EngineConfig& {
        return Cfg;
    }

  private:
    EngineConfig Cfg;               ///< Populated by LoadFile; all fields start nullopt.
    Path         m_EngineDirPath;   ///< Engine root, set by Init().
    Path         m_ShadersDirPath;  ///< Engine/Shaders/, pre-computed by Init().
    Path         m_LogsDirPath;     ///< Engine/Logs/, pre-computed by Init().
    Path         m_ConfigsDirPath;  ///< Engine/Configs/, pre-computed by Init().
    Path         m_BinariesDirPath; ///< Engine/Binaries/, pre-computed by Init().
};
} // namespace SoulEngine::Core
