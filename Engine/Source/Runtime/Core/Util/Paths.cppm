export module Core:Util.Paths;

import :Util.Types;
import :Util.Singleton;

namespace SoulEngine {

/// @brief Well-known engine directory paths.
///
/// Pure path provider split out of the retired ConfigManager: it answers "where is X" and nothing else. Paths
/// are engine-environment facts, not configuration values — runtime-tunable settings belong to CVarManager.
///
/// Usage: ~~~{.cpp}
///     EnginePaths::Get().Init(EngineDir);   // once, at startup
///     const Path& Shaders = EnginePaths::Get().EngineShadersDirPath();
/// ~~~
export class EnginePaths final : public Singleton<EnginePaths> {
    friend class Singleton<EnginePaths>;
    EnginePaths() = default;

  public:
    /// @brief Initialize with the engine root directory.
    /// @details Stores the engine root and pre-computes well-known
    ///          sub-directory paths (Shaders, Logs, Configs, Binaries,
    ///          ApplicationsRoot).  No I/O.
    /// @param EngineDirPath Absolute path to the engine root (the
    ///        directory containing Configs/, Shaders/, Logs/, etc.).
    auto Init(const Path& EngineDirPath) -> void;

    /// @brief Access the engine root directory.
    [[nodiscard]] auto EngineDirPath() const -> const Path&;

    /// @brief Path to Engine/Shaders/.
    [[nodiscard]] auto EngineShadersDirPath() const -> const Path&;

    /// @brief Path to Engine/Logs/.
    [[nodiscard]] auto LogsDirPath() const -> const Path&;

    /// @brief Path to Engine/Configs/.
    [[nodiscard]] auto ConfigsDirPath() const -> const Path&;

    /// @brief Path to Engine/Binaries/.
    [[nodiscard]] auto BinariesDirPath() const -> const Path&;

    /// @brief Path to the Applications root directory.
    /// @details Computed as EngineDirPath().parent_path() / "Applications".
    [[nodiscard]] auto ApplicationsRootDirPath() const -> const Path&;

  private:
    Path m_EngineDirPath;           ///< Engine root, set by Init().
    Path m_EngineShadersDirPath;    ///< Engine/Shaders/, pre-computed by Init().
    Path m_LogsDirPath;             ///< Engine/Logs/, pre-computed by Init().
    Path m_ConfigsDirPath;          ///< Engine/Configs/, pre-computed by Init().
    Path m_BinariesDirPath;         ///< Engine/Binaries/, pre-computed by Init().
    Path m_ApplicationsRootDirPath; ///< Applications/, pre-computed by Init().
};
} // namespace SoulEngine
