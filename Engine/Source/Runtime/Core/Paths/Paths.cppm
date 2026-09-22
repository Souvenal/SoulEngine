export module Core:Paths;

export import :Util;
export import std;

export namespace SoulEngine {

/// @brief Well-known engine directory paths.
///
/// Pure path provider split out of the retired ConfigManager: it answers
/// "where is X" and nothing else.  Paths are engine-environment facts, not
/// configuration values — runtime-tunable settings belong to CVarManager.
///
/// Usage:
/// ~~~{.cpp}
///     EnginePaths::Get().Init(EngineDir);   // once, at startup
///     const Path& Shaders = EnginePaths::Get().EngineShadersDirPath();
/// ~~~
class EnginePaths final : public Singleton<EnginePaths> {
    friend class Singleton<EnginePaths>;
    EnginePaths() = default;

  public:
    /// @brief Initialize with the engine root directory.
    /// @details Stores the engine root and pre-computes well-known
    ///          sub-directory paths (Shaders, Logs, Configs, Binaries,
    ///          ApplicationsRoot).  No I/O.
    /// @param EngineDirPath Absolute path to the engine root (the
    ///        directory containing Configs/, Shaders/, Logs/, etc.).
    auto Init(const Path& EngineDirPath) -> void {
        m_EngineDirPath           = EngineDirPath;
        m_EngineShadersDirPath    = EngineDirPath / "Shaders";
        m_LogsDirPath             = EngineDirPath / "Logs";
        m_ConfigsDirPath          = EngineDirPath / "Configs";
        m_BinariesDirPath         = EngineDirPath / "Binaries";
        m_ApplicationsRootDirPath = EngineDirPath.parent_path() / "Applications";
    }

    /// @brief Access the engine root directory.
    [[nodiscard]] auto EngineDirPath() const -> const Path& {
        return m_EngineDirPath;
    }

    /// @brief Path to Engine/Shaders/.
    [[nodiscard]] auto EngineShadersDirPath() const -> const Path& {
        return m_EngineShadersDirPath;
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

    /// @brief Path to the Applications root directory.
    /// @details Computed as EngineDirPath().parent_path() / "Applications".
    [[nodiscard]] auto ApplicationsRootDirPath() const -> const Path& {
        return m_ApplicationsRootDirPath;
    }

  private:
    Path m_EngineDirPath;           ///< Engine root, set by Init().
    Path m_EngineShadersDirPath;    ///< Engine/Shaders/, pre-computed by Init().
    Path m_LogsDirPath;             ///< Engine/Logs/, pre-computed by Init().
    Path m_ConfigsDirPath;          ///< Engine/Configs/, pre-computed by Init().
    Path m_BinariesDirPath;         ///< Engine/Binaries/, pre-computed by Init().
    Path m_ApplicationsRootDirPath; ///< Applications/, pre-computed by Init().
};
} // namespace SoulEngine
