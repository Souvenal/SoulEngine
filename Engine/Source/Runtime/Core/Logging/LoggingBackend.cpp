// This is intentionally a conventional translation unit, not a C++ module.
//
// MSVC rejects a module unit that textually includes spdlog (and the standard
// library headers it includes) and also imports the named std module: the two
// representations of the standard library produce duplicate/conflicting
// declarations.  Keep every spdlog include and implementation detail here.
// Core:Logging imports std safely and reaches this private backend through the
// C ABI declared in Logging.cppm.

#include <spdlog/common.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

enum class BackendLogLevel : int {
    Unknown = 0,
    Debug,
    Info,
    Warning,
    Error,
};

enum class BackendLogThreadRole : int {
    Unknown = 0,
    Main,
    Game,
    Render,
    RHI,
    Worker,
};

thread_local BackendLogThreadRole CurrentLogThreadRole = BackendLogThreadRole::Main;

[[nodiscard]] auto ToBackendLogLevel(int Level) -> BackendLogLevel {
    switch (Level) {
    case 0:
        return BackendLogLevel::Debug;
    case 1:
        return BackendLogLevel::Info;
    case 2:
        return BackendLogLevel::Warning;
    case 3:
        return BackendLogLevel::Error;
    }
    return BackendLogLevel::Unknown;
}

[[nodiscard]] auto ToSpdlogLevel(BackendLogLevel Level) -> spdlog::level::level_enum {
    switch (Level) {
    case BackendLogLevel::Debug:
        return spdlog::level::debug;
    case BackendLogLevel::Info:
        return spdlog::level::info;
    case BackendLogLevel::Warning:
        return spdlog::level::warn;
    case BackendLogLevel::Error:
        return spdlog::level::err;
    case BackendLogLevel::Unknown:
        return spdlog::level::trace;
    }
    return spdlog::level::trace;
}

[[nodiscard]] auto GetLogThreadName() -> std::string_view {
    switch (CurrentLogThreadRole) {
    case BackendLogThreadRole::Main:
        return "Main";
    case BackendLogThreadRole::Game:
        return "Game";
    case BackendLogThreadRole::Render:
        return "Render";
    case BackendLogThreadRole::RHI:
        return "RHI";
    case BackendLogThreadRole::Worker:
        return "Worker";
    case BackendLogThreadRole::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

[[nodiscard]] auto GetLogThreadColor() -> std::string_view {
    switch (CurrentLogThreadRole) {
    case BackendLogThreadRole::Main:
        return "\033[97m";
    case BackendLogThreadRole::Game:
        return "\033[32m";
    case BackendLogThreadRole::Render:
        return "\033[36m";
    case BackendLogThreadRole::RHI:
        return "\033[35m";
    case BackendLogThreadRole::Worker:
        return "\033[33m";
    case BackendLogThreadRole::Unknown:
        return "\033[90m";
    }
    return "\033[90m";
}

[[nodiscard]] auto CenterLogText(std::string_view Text, std::size_t Width) -> std::string {
    if (Text.size() >= Width)
        return std::string{Text};

    const auto Padding = Width - Text.size();
    const auto Left    = Padding / 2;
    const auto Right   = Padding - Left;
    return std::string(Left, ' ') + std::string(Text) + std::string(Right, ' ');
}

[[nodiscard]] auto FormatLogThreadRole(bool Color) -> std::string {
    constexpr std::size_t RoleWidth = 6;

    const auto RoleName = CenterLogText(GetLogThreadName(), RoleWidth);

    if (!Color)
        return "[" + RoleName + "]";

    return std::string(GetLogThreadColor()) + "[" + RoleName + "]\033[0m";
}

class ThreadRoleFormatter final : public spdlog::custom_flag_formatter {
  public:
    explicit ThreadRoleFormatter(bool Color)
        : m_Color(Color) {}

    auto format(const spdlog::details::log_msg&, const std::tm&, spdlog::memory_buf_t& Dest) -> void override {
        const auto RoleText = FormatLogThreadRole(m_Color);
        Dest.append(RoleText.data(), RoleText.data() + RoleText.size());
    }

    [[nodiscard]] auto clone() const -> std::unique_ptr<spdlog::custom_flag_formatter> override {
        return std::make_unique<ThreadRoleFormatter>(m_Color);
    }

  private:
    bool m_Color = false;
};

[[nodiscard]] auto CreateLogFormatter(std::string Pattern, bool ColorThreadRole) -> std::unique_ptr<spdlog::formatter> {
    auto Formatter = std::make_unique<spdlog::pattern_formatter>();
    Formatter->add_flag<ThreadRoleFormatter>('q', ColorThreadRole);
    Formatter->set_pattern(std::move(Pattern));
    return Formatter;
}

struct BackendState {
    spdlog::sinks::sink*  EditorSink  = nullptr;
    spdlog::sinks::sink*  ConsoleSink = nullptr;
    std::unique_ptr<spdlog::logger> Logger;
};

[[nodiscard]] auto GetBackendState() -> BackendState& {
    static BackendState State;
    return State;
}

auto WriteLog(BackendLogLevel Level, std::string_view Message) -> void {
    auto& State = GetBackendState();
    if (!State.Logger)
        return;

    switch (Level) {
    case BackendLogLevel::Debug:
        State.Logger->debug(Message);
        break;
    case BackendLogLevel::Info:
        State.Logger->info(Message);
        break;
    case BackendLogLevel::Warning:
        State.Logger->warn(Message);
        break;
    case BackendLogLevel::Error:
        State.Logger->error(Message);
        break;
    case BackendLogLevel::Unknown:
        break;
    }
}

[[nodiscard]] auto ParseLogLevel(const char* Name) -> std::optional<BackendLogLevel> {
    if (Name == nullptr)
        return std::nullopt;

    const std::string_view Value = Name;
    if (Value == "Debug")
        return BackendLogLevel::Debug;
    if (Value == "Info")
        return BackendLogLevel::Info;
    if (Value == "Warning")
        return BackendLogLevel::Warning;
    if (Value == "Error")
        return BackendLogLevel::Error;
    return std::nullopt;
}

auto ApplySinkLevel(spdlog::sinks::sink* Sink, const char* ConfiguredLevel, BackendLogLevel DefaultLevel) -> void {
    if (Sink == nullptr)
        return;

    auto Level = DefaultLevel;
    if (ConfiguredLevel != nullptr) {
        if (const auto Parsed = ParseLogLevel(ConfiguredLevel))
            Level = *Parsed;
        else
            WriteLog(BackendLogLevel::Warning, "Unknown log level '" + std::string(ConfiguredLevel) + "', using default");
    }
    Sink->set_level(ToSpdlogLevel(Level));
}

} // namespace

extern "C" auto SoulEngineLoggingBackendInit(const char* LogDirectory) -> void {
    auto& State = GetBackendState();
    if (State.Logger)
        return;

    std::filesystem::create_directories(LogDirectory);

    // Create file sink — logger will hold its own shared_ptr copy.
    auto FileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        (std::filesystem::path(LogDirectory) / "SoulEngine.log").string(), true);
    State.EditorSink = FileSink.get();

    auto ConsoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    State.ConsoleSink = ConsoleSink.get();

    FileSink->set_formatter(CreateLogFormatter("[%m-%d %H:%M:%S.%e] [%=7l] %q %v", false));
    ConsoleSink->set_formatter(CreateLogFormatter("[%H:%M:%S.%e] [%^%=7l%$] %q %v", true));

    // Build the sink list with the shared_ptrs that spdlog's API requires.
    std::vector<std::shared_ptr<spdlog::sinks::sink>> Sinks;
    Sinks.emplace_back(FileSink);
    Sinks.emplace_back(ConsoleSink);

    State.Logger = std::make_unique<spdlog::logger>("SoulEngine", Sinks.begin(), Sinks.end());
    State.Logger->set_level(spdlog::level::trace);
    State.Logger->set_error_handler([](const std::string&) noexcept {});
}

extern "C" auto SoulEngineLoggingBackendSetThreadRole(int Role) -> void {
    CurrentLogThreadRole = static_cast<BackendLogThreadRole>(Role);
}

extern "C" auto SoulEngineLoggingBackendWrite(int Level, const char* Message) -> void {
    WriteLog(ToBackendLogLevel(Level), Message);
}

extern "C" auto SoulEngineLoggingBackendSetSinkLevels(const char* FileLevel, const char* ConsoleLevel) -> void {
    auto& State = GetBackendState();
    ApplySinkLevel(State.EditorSink, FileLevel, BackendLogLevel::Debug);
    ApplySinkLevel(State.ConsoleSink, ConsoleLevel, BackendLogLevel::Info);
}

extern "C" auto SoulEngineLoggingBackendWriteFile(const char* Filename, int Level, const char* Message) -> void {
    static std::atomic<unsigned int> FileCounter = 0;
    // Create file sink — logger will hold its own shared_ptr copy.
    auto FileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(Filename, true);
    auto TempLogger = std::make_shared<spdlog::logger>(
        "file_" + std::to_string(FileCounter.fetch_add(1, std::memory_order_relaxed)), FileSink);
    TempLogger->set_formatter(CreateLogFormatter("[%m-%d %H:%M:%S.%e] [%=7l] %q %v", false));

    switch (ToBackendLogLevel(Level)) {
    case BackendLogLevel::Debug:
        TempLogger->debug(Message);
        break;
    case BackendLogLevel::Info:
        TempLogger->info(Message);
        break;
    case BackendLogLevel::Warning:
        TempLogger->warn(Message);
        break;
    case BackendLogLevel::Error:
        TempLogger->error(Message);
        break;
    case BackendLogLevel::Unknown:
        break;
    }
}
