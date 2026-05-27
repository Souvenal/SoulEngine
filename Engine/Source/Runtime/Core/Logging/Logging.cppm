module;

#include <spdlog/common.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

export module Core:Logging;

export import :Util;

export import std;

namespace SoulEngine::Core {
export enum class LogLevel : Uint8 { Debug = 0, Info = 1, Warning = 2, Error = 3 };

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
        std::filesystem::create_directories(LogDirPath);

        // Create file sink — logger will hold its own shared_ptr copy.
        auto FileSink =
            std::make_shared<spdlog::sinks::basic_file_sink_mt>((LogDirPath / "SoulEngine.log").string(), true);
        m_EditorSink = FileSink.get();

        auto ConsoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        m_ConsoleSink    = ConsoleSink.get();

        // Build the sink list with the shared_ptrs that spdlog's API requires.
        std::vector<SPtr<spdlog::sinks::sink>> Sinks;
        Sinks.emplace_back(FileSink);
        Sinks.emplace_back(ConsoleSink);

        Impl = std::make_unique<spdlog::logger>("SoulEngine", Sinks.begin(), Sinks.end());
        Impl->set_level(spdlog::level::trace);
        Impl->set_error_handler([](const String&) noexcept {});
    }

    LogManager(const LogManager&)            = delete;
    LogManager& operator=(const LogManager&) = delete;
    LogManager(LogManager&&)                 = delete;
    LogManager& operator=(LogManager&&)      = delete;

    auto Log(LogLevel Level, StringView Message) -> void {
        if (!Impl)
            return;

        switch (Level) {
        case LogLevel::Debug:
            Impl->debug(Message);
            break;
        case LogLevel::Info:
            Impl->info(Message);
            break;
        case LogLevel::Warning:
            Impl->warn(Message);
            break;
        case LogLevel::Error:
            Impl->error(Message);
            break;
        }
    }

    /// @brief Set minimum log level per sink type from config-level names.
    /// @param FileLevel    String name for file sink ("Debug"|"Info"|"Warning"|"Error",
    ///                     nullopt → Debug).
    /// @param ConsoleLevel String name for console sink (nullopt → Info).
    /// @note The logger-level is kept at `trace` so that each sink's
    ///       individual level is the sole gate.
    auto SetSinkLevels(const std::optional<String>& FileLevel, const std::optional<String>& ConsoleLevel) -> void {
        auto ParseLevel = [](StringView Name) -> std::optional<LogLevel> {
            if (Name == "Debug")
                return LogLevel::Debug;
            if (Name == "Info")
                return LogLevel::Info;
            if (Name == "Warning")
                return LogLevel::Warning;
            if (Name == "Error")
                return LogLevel::Error;
            return std::nullopt;
        };

        auto ToSpdlog = [](LogLevel L) -> spdlog::level::level_enum {
            switch (L) {
            case LogLevel::Debug:
                return spdlog::level::debug;
            case LogLevel::Info:
                return spdlog::level::info;
            case LogLevel::Warning:
                return spdlog::level::warn;
            case LogLevel::Error:
                return spdlog::level::err;
            }
            return spdlog::level::trace;
        };

        auto Apply = [&](spdlog::sinks::sink* Sink, const std::optional<String>& CfgName, LogLevel Default) -> void {
            if (!Sink)
                return;
            LogLevel L = Default;
            if (CfgName) {
                if (auto Parsed = ParseLevel(*CfgName))
                    L = *Parsed;
                else
                    Log(LogLevel::Warning, std::format("Unknown log level '{}', using default", *CfgName));
            }
            Sink->set_level(ToSpdlog(L));
        };

        Apply(m_EditorSink, FileLevel, LogLevel::Debug);
        Apply(m_ConsoleSink, ConsoleLevel, LogLevel::Info);
    }

  private:
    spdlog::sinks::sink* m_EditorSink = nullptr;
    spdlog::sinks::sink* m_ConsoleSink = nullptr;
    UPtr<spdlog::logger> Impl;
};

export template <typename... Args>
auto LogFormatted(LogLevel Level, StringView FormatStr, const Args&... InArgs) -> void {
    String Msg = std::vformat(FormatStr, std::make_format_args(InArgs...));
    LogManager::Get().Log(Level, Msg);
}

export template <typename... Args>
auto LogToFileFormatted(StringView Filename, LogLevel Level, StringView FormatStr, const Args&... InArgs) -> void {
    String                     Msg         = std::vformat(FormatStr, std::make_format_args(InArgs...));
    static std::atomic<Uint32> FileCounter = 0;
    auto                       FileSink   = std::make_shared<spdlog::sinks::basic_file_sink_mt>(String(Filename), true);
    auto                       TempLogger = std::make_shared<spdlog::logger>(
        "file_" + std::to_string(FileCounter.fetch_add(1, std::memory_order_relaxed)), FileSink);
    switch (Level) {
    case LogLevel::Debug:
        TempLogger->debug(Msg);
        break;
    case LogLevel::Info:
        TempLogger->info(Msg);
        break;
    case LogLevel::Warning:
        TempLogger->warn(Msg);
        break;
    case LogLevel::Error:
        TempLogger->error(Msg);
        break;
    }
}
} // namespace SoulEngine::Core

using namespace SoulEngine::Core;

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
