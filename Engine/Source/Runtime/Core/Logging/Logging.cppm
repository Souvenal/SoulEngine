/// @file   Logging/Logging.cppm
/// @brief  spdlog-backed logging: LogManager singleton, log level and
///         thread-role enums, and the LogDebug/LogInfo/... entry points.

module;

// spdlog is textually included in the global module fragment.  The toolchain
// is Clang-only, which handles mixing header-provided standard-library
// declarations with the named std module in one translation unit.  GMF types
// used by the exported interface (the LogManager members below and the
// bodies of its inline member functions) are retained per
// [module.global.frag]/3 and stay reachable-but-not-nameable for importers
// ([module.reach]/3.1.2, /6).
#include <spdlog/common.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

export module Core:Logging;

export import :Util;
export import std;

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

// Non-exported module-scope state: these are odr-used by exported inline
// functions below, so they live in a plain (non-exported) namespace scope,
// not an anonymous namespace — see Core:CVar's linkage note.
thread_local LogThreadRole CurrentLogThreadRole = LogThreadRole::Main;

export auto SetLogThreadRole(LogThreadRole Role) -> void {
    CurrentLogThreadRole = Role;
}

[[nodiscard]] auto GetLogThreadColor() -> StringView {
    switch (CurrentLogThreadRole) {
    case LogThreadRole::Main:
        return "\033[97m";
    case LogThreadRole::Game:
        return "\033[32m";
    case LogThreadRole::Render:
        return "\033[36m";
    case LogThreadRole::RHI:
        return "\033[35m";
    case LogThreadRole::Worker:
        return "\033[33m";
    case LogThreadRole::Unknown:
        return "\033[90m";
    }
    return "\033[90m";
}

[[nodiscard]] auto CenterLogText(StringView Text, std::size_t Width) -> String {
    if (Text.size() >= Width)
        return String{Text};

    const auto Padding = Width - Text.size();
    const auto Left    = Padding / 2;
    const auto Right   = Padding - Left;
    return String(Left, ' ') + String(Text) + String(Right, ' ');
}

[[nodiscard]] auto FormatLogThreadRole(bool Color) -> String {
    constexpr std::size_t RoleWidth = 6;

    const auto RoleName = CenterLogText(magic_enum::enum_name(CurrentLogThreadRole), RoleWidth);

    if (!Color)
        return "[" + RoleName + "]";

    return String(GetLogThreadColor()) + "[" + RoleName + "]\033[0m";
}

class ThreadRoleFormatter final : public spdlog::custom_flag_formatter {
  public:
    explicit ThreadRoleFormatter(bool Color)
        : m_Color(Color) {}

    auto format(const spdlog::details::log_msg&, const std::tm&, spdlog::memory_buf_t& Dest) -> void override {
        const auto RoleText = FormatLogThreadRole(m_Color);
        Dest.append(RoleText.data(), RoleText.data() + RoleText.size());
    }

    [[nodiscard]] auto clone() const -> UPtr<spdlog::custom_flag_formatter> override {
        return std::make_unique<ThreadRoleFormatter>(m_Color);
    }

  private:
    bool m_Color = false;
};

[[nodiscard]] auto CreateLogFormatter(String Pattern, bool ColorThreadRole) -> UPtr<spdlog::formatter> {
    auto Formatter = std::make_unique<spdlog::pattern_formatter>();
    Formatter->add_flag<ThreadRoleFormatter>('q', ColorThreadRole);
    Formatter->set_pattern(std::move(Pattern));
    return Formatter;
}

[[nodiscard]] auto ToSpdlogLevel(LogLevel Level) -> spdlog::level::level_enum {
    switch (Level) {
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
}

[[nodiscard]] auto ParseLogLevel(StringView Name) -> std::optional<LogLevel> {
    return magic_enum::enum_cast<LogLevel>(Name);
}

auto ApplySinkLevel(spdlog::logger* Logger, spdlog::sinks::sink* Sink, StringView ConfiguredLevel, LogLevel DefaultLevel) -> void {
    if (Sink == nullptr)
        return;

    auto Level = DefaultLevel;
    if (!ConfiguredLevel.empty()) {
        if (const auto Parsed = ParseLogLevel(ConfiguredLevel)) {
            Level = *Parsed;
        } else if (Logger != nullptr) {
            Logger->warn(Format("Unknown log level '{}', using default", ConfiguredLevel));
        }
    }
    Sink->set_level(ToSpdlogLevel(Level));
}

export class LogManager final : public Singleton<LogManager> {
    friend class Singleton<LogManager>;

    // spdlog types live in the global module fragment, so importers cannot
    // name these member types — they stay reachable-but-not-nameable via the
    // BMI ([module.reach]/6), which is exactly the encapsulation we want.
    UPtr<spdlog::logger> m_Logger      = nullptr;
    spdlog::sinks::sink* m_EditorSink  = nullptr;
    spdlog::sinks::sink* m_ConsoleSink = nullptr;

    // Defined out-of-class below: an implicitly-defined constructor or
    // destructor of an exported class needs the complete type of every
    // member, and importer TUs cannot emit that code from the BMI — only
    // this TU can.  (Bodies that merely *call* spdlog members, like Init
    // or Log, inline fine.)
    LogManager();

  public:
    // Defaulted (out-of-class) rather than calling spdlog::shutdown():
    // spdlog's internal registry is a function-local static created during
    // Init(), i.e. after this singleton, so it is destroyed before us —
    // shutdown() here would touch the registry's already-destroyed mutex.
    ~LogManager();

    /// @brief Initialize spdlog sinks and logger.
    /// @details Creates LogDirPath if needed, sets up file and console
    ///          sinks, and registers the logger.  Must be called once
    ///          before any log output.
    /// @param LogDirPath Directory for the log file (e.g. Engine/Logs/).
    auto Init(const Path& LogDirPath) -> void {
        if (m_Logger)
            return;

        std::filesystem::create_directories(LogDirPath);

        // Create file sink — logger will hold its own shared_ptr copy.
        auto FileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((LogDirPath / "SoulEngine.log").string(), true);
        m_EditorSink  = FileSink.get();

        auto ConsoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        m_ConsoleSink    = ConsoleSink.get();

        FileSink->set_formatter(CreateLogFormatter("[%m-%d %H:%M:%S.%e] [%=7l] %q %v", false));
        ConsoleSink->set_formatter(CreateLogFormatter("[%H:%M:%S.%e] [%^%=7l%$] %q %v", true));

        // Build the sink list with the shared_ptrs that spdlog's API requires.
        std::vector<SPtr<spdlog::sinks::sink>> Sinks;
        Sinks.emplace_back(FileSink);
        Sinks.emplace_back(ConsoleSink);

        m_Logger = std::make_unique<spdlog::logger>("SoulEngine", Sinks.begin(), Sinks.end());
        m_Logger->set_level(spdlog::level::trace);
        m_Logger->set_error_handler([](const String&) noexcept {});
    }

    LogManager(const LogManager&)            = delete;
    LogManager& operator=(const LogManager&) = delete;
    LogManager(LogManager&&)                 = delete;
    LogManager& operator=(LogManager&&)      = delete;

    auto Log(LogLevel Level, StringView Message) -> void {
        if (!m_Logger)
            return;

        switch (Level) {
        case LogLevel::Debug:
            m_Logger->debug(Message);
            break;
        case LogLevel::Info:
            m_Logger->info(Message);
            break;
        case LogLevel::Warning:
            m_Logger->warn(Message);
            break;
        case LogLevel::Error:
            m_Logger->error(Message);
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
        ApplySinkLevel(m_Logger.get(), m_EditorSink, FileLevel ? StringView{*FileLevel} : StringView{}, LogLevel::Debug);
        ApplySinkLevel(m_Logger.get(), m_ConsoleSink, ConsoleLevel ? StringView{*ConsoleLevel} : StringView{}, LogLevel::Info);
    }

    auto LogToFile(LogLevel Level, StringView Filename, StringView Message) -> void {
        static std::atomic<Uint32> FileCounter = 0;

        // Create file sink — logger will hold its own shared_ptr copy.
        auto FileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(String{Filename}, true);
        auto TempLogger = std::make_shared<spdlog::logger>(
            "file_" + std::to_string(FileCounter.fetch_add(1, std::memory_order_relaxed)), FileSink);
        TempLogger->set_formatter(CreateLogFormatter("[%m-%d %H:%M:%S.%e] [%=7l] %q %v", false));

        switch (Level) {
        case LogLevel::Debug:
            TempLogger->debug(Message);
            break;
        case LogLevel::Info:
            TempLogger->info(Message);
            break;
        case LogLevel::Warning:
            TempLogger->warn(Message);
            break;
        case LogLevel::Error:
            TempLogger->error(Message);
            break;
        }
    }
};

// Non-inline definitions; see the declarations inside LogManager.
LogManager::LogManager()  = default;
LogManager::~LogManager() = default;

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
