module Core:Util.Paths.impl;

import :Util.Paths;

namespace SoulEngine {

auto EnginePaths::Init(const Path& EngineDirPath) -> void {
    m_EngineDirPath           = EngineDirPath;
    m_EngineShadersDirPath    = EngineDirPath / "Shaders";
    m_LogsDirPath             = EngineDirPath / "Logs";
    m_ConfigsDirPath          = EngineDirPath / "Configs";
    m_BinariesDirPath         = EngineDirPath / "Binaries";
    m_ApplicationsRootDirPath = EngineDirPath.parent_path() / "Applications";
}

auto EnginePaths::EngineDirPath() const -> const Path& {
    return m_EngineDirPath;
}
auto EnginePaths::EngineShadersDirPath() const -> const Path& {
    return m_EngineShadersDirPath;
}
auto EnginePaths::LogsDirPath() const -> const Path& {
    return m_LogsDirPath;
}
auto EnginePaths::ConfigsDirPath() const -> const Path& {
    return m_ConfigsDirPath;
}
auto EnginePaths::BinariesDirPath() const -> const Path& {
    return m_BinariesDirPath;
}
auto EnginePaths::ApplicationsRootDirPath() const -> const Path& {
    return m_ApplicationsRootDirPath;
}

} // namespace SoulEngine
