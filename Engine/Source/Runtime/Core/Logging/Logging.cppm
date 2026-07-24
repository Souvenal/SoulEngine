export module Core:Logging;

export import :Util;
export import std;

// Keep spdlog out of this module interface.  On MSVC, including spdlog's
// transitive standard-library headers in a module unit and then importing std
// produces duplicate/conflicting standard-library declarations.  The logging
// backend is therefore a conventional .cpp translation unit; these are its
// private C ABI bridge functions, not part of Core:Logging's exported API.
extern "C" auto SoulEngineLoggingBackendInit(const char* LogDirectory) -> void;
extern "C" auto SoulEngineLoggingBackendSetThreadRole(int Role) -> void;
extern "C" auto SoulEngineLoggingBackendWrite(int Level, const char* Message) -> void;
extern "C" auto SoulEngineLoggingBackendSetSinkLevels(const char* FileLevel, const char* ConsoleLevel) -> void;
extern "C" auto SoulEngineLoggingBackendWriteFile(const char* Filename, int Level, const char* Message) -> void;

namespace SoulEngine {
export enum class LogLevel : Uint8 { Debug = 0, Info = 1, Warning = 2, Error = 3 };

export enum class LogThreadRole : Uint8 {
    Unknown = 0,
    Main,
    Game,
    Render,
    RHI,
    Worker,
};

namespace {
thread_local LogThreadRole CurrentLogThreadRole = LogThreadRole::Main;
}

export auto SetLogThreadRole(LogThreadRole Role) -> void {
    CurrentLogThreadRole = Role;
    SoulEngineLoggingBackendSetThreadRole(static_cast<int>(Role));
}

export [[nodiscard]] auto GetLogThreadName() -> StringView {
    switch (CurrentLogThreadRole) {
    case LogThreadRole::Main:
        return "Main";
    case LogThreadRole::Game:
        return "Game";
    case LogThreadRole::Render:
        return "Render";
    case LogThreadRole::RHI:
        return "RHI";
    case LogThreadRole::Worker:
        return "Worker";
    case LogThreadRole::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

export class LogManager final : public Singleton<LogManager> {
    friend class Singleton<LogManager>;

    LogManager() = default;

  public:
    ~LogManager() {
        // Intentionally empty. spdlog's internal registry is a function-
        // local static that is destroyed before the LogManager singleton
        // (registry was constructed later during Init()).  Calling
        // shutdown() here would crash on the registry's already-destroyed
        // mutex.  The UPtr members clean up naturally.
    }

    /// @brief Initialize spdlog sinks and logger.
    /// @details Creates LogDirPath if needed, sets up file and console
    ///          sinks, and registers the logger.  Must be called once
    ///          before any log output.
    /// @param LogDirPath Directory for the log file (e.g. Engine/Logs/).
    auto Init(const Path& LogDirPath) -> void {
        const auto LogDirectory = LogDirPath.string();
        SoulEngineLoggingBackendInit(LogDirectory.c_str());
    }

    LogManager(const LogManager&)            = delete;
    LogManager& operator=(const LogManager&) = delete;
    LogManager(LogManager&&)                 = delete;
    LogManager& operator=(LogManager&&)      = delete;

    auto Log(LogLevel Level, StringView Message) -> void {
        const String OwnedMessage{Message};
        SoulEngineLoggingBackendWrite(static_cast<int>(Level), OwnedMessage.c_str());
    }

    /// @brief Set minimum log level per sink type from config-level names.
    /// @param FileLevel    String name for file sink ("Debug"|"Info"|"Warning"|"Error",
    ///                     nullopt → Debug).
    /// @param ConsoleLevel String name for console sink (nullopt → Info).
    /// @note The logger-level is kept at `trace` so that each sink's
    ///       individual level is the sole gate.
    auto SetSinkLevels(const std::optional<String>& FileLevel, const std::optional<String>& ConsoleLevel) -> void {
        SoulEngineLoggingBackendSetSinkLevels(FileLevel ? FileLevel->c_str() : nullptr,
                                              ConsoleLevel ? ConsoleLevel->c_str() : nullptr);
    }

    auto LogToFile(LogLevel Level, StringView Filename, StringView Message) -> void {
        const String OwnedFilename{Filename};
        const String OwnedMessage{Message};
        SoulEngineLoggingBackendWriteFile(OwnedFilename.c_str(), static_cast<int>(Level), OwnedMessage.c_str());
    }
};

export template <typename... Args>
auto LogFormatted(LogLevel Level, StringView FormatStr, const Args&... InArgs) -> void {
    String Msg = std::vformat(FormatStr, std::make_format_args(InArgs...));
    LogManager::Get().Log(Level, Msg);
}

export template <typename... Args>
auto LogToFileFormatted(StringView Filename, LogLevel Level, StringView FormatStr, const Args&... InArgs) -> void {
    String Msg = std::vformat(FormatStr, std::make_format_args(InArgs...));
    LogManager::Get().LogToFile(Level, Filename, Msg);
}

export template <typename... Args>
auto LogDebug(StringView Format, const Args&... InArgs) -> void {
    LogFormatted(LogLevel::Debug, Format, InArgs...);
}

export template <typename... Args>
auto LogInfo(StringView Format, const Args&... InArgs) -> void {
    LogFormatted(LogLevel::Info, Format, InArgs...);
}

export template <typename... Args>
auto LogWarning(StringView Format, const Args&... InArgs) -> void {
    LogFormatted(LogLevel::Warning, Format, InArgs...);
}

export template <typename... Args>
auto LogError(StringView Format, const Args&... InArgs) -> void {
    LogFormatted(LogLevel::Error, Format, InArgs...);
}

export template <typename... Args>
auto LogToFile(StringView Filename, LogLevel Level, StringView Format, const Args&... InArgs) -> void {
    LogToFileFormatted(Filename, Level, Format, InArgs...);
}
} // namespace SoulEngine
