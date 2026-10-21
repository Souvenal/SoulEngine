/// @file   Logging/Logging.cppm
/// @brief  spdlog-backed logging: LogManager singleton, log level and
///         thread-role enums, and the LogDebug/LogInfo/... entry points.

module;

#include <spdlog/common.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

export module Core.Util.Logging;

import Core.Util.Types;
import Core.Util.Singleton;
import std;

import magic_enum;

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

export auto SetLogThreadRole(LogThreadRole Role) -> void;

export class LogManager final : public Singleton<LogManager> {
    friend class Singleton<LogManager>;

    // spdlog types live in the global module fragment, so importers cannot name these member types — they
    // stay reachable-but-not-nameable via the BMI ([module.reach]/6), which is exactly the encapsulation we
    // want.
    UPtr<spdlog::logger> m_Logger      = nullptr;
    spdlog::sinks::sink* m_EditorSink  = nullptr;
    spdlog::sinks::sink* m_ConsoleSink = nullptr;

    // Defined out-of-class below: an implicitly-defined constructor or destructor of an exported class needs
    // the complete type of every member, and importer TUs cannot emit that code from the BMI — only this TU
    // can. (Bodies that merely *call* spdlog members, like Init or Log, inline fine.)
    LogManager();

  public:
    // Defaulted (out-of-class) rather than calling spdlog::shutdown(): spdlog's internal registry is a
    // function-local static created during Init(), i.e. after this singleton, so it is destroyed before us —
    // shutdown() here would touch the registry's already-destroyed mutex.
    ~LogManager();

    /// @brief Initialize spdlog sinks and logger.
    /// @details Creates LogDirPath if needed, sets up file and console
    ///          sinks, and registers the logger.  Must be called once
    ///          before any log output.
    /// @param LogDirPath Directory for the log file (e.g. Engine/Logs/).
    auto Init(const Path& LogDirPath) -> void;

    LogManager(const LogManager&)            = delete;
    LogManager& operator=(const LogManager&) = delete;
    LogManager(LogManager&&)                 = delete;
    LogManager& operator=(LogManager&&)      = delete;

    auto Log(LogLevel Level, StringView Message) -> void;

    /// @brief Set minimum log level per sink type from the Log.FileLevel and Log.ConsoleLevel CVars.
    /// @note The logger-level is kept at `trace` so that each sink's
    ///       individual level is the sole gate.
    auto SetSinkLevels() -> void;

    auto LogToFile(LogLevel Level, StringView Filename, StringView Message) -> void;
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
