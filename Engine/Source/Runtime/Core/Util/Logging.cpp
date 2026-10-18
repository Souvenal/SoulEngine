module;

#include <spdlog/common.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

module Core:Util.Logging.impl;

import :Util.Logging;
import magic_enum;

namespace SoulEngine {

thread_local LogThreadRole CurrentLogThreadRole = LogThreadRole::Main;

auto SetLogThreadRole(LogThreadRole Role) -> void {
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
    const auto            RoleName  = CenterLogText(magic_enum::enum_name(CurrentLogThreadRole), RoleWidth);
    if (!Color)
        return "[" + RoleName + "]";
    return String(GetLogThreadColor()) + "[" + RoleName + "]\033[0m";
}

class ThreadRoleFormatter final : public spdlog::custom_flag_formatter {
  public:
    explicit ThreadRoleFormatter(bool Color) : m_Color(Color) {}
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

auto ApplySinkLevel(spdlog::logger*      Logger,
                    spdlog::sinks::sink* Sink,
                    StringView           ConfiguredLevel,
                    LogLevel             DefaultLevel) -> void {
    if (Sink == nullptr)
        return;
    auto Level = DefaultLevel;
    if (!ConfiguredLevel.empty()) {
        if (const auto Parsed = ParseLogLevel(ConfiguredLevel))
            Level = *Parsed;
        else if (Logger != nullptr)
            Logger->warn(Format("Unknown log level '{}', using default", ConfiguredLevel));
    }
    Sink->set_level(ToSpdlogLevel(Level));
}

LogManager::LogManager()  = default;
LogManager::~LogManager() = default;

auto LogManager::Init(const Path& LogDirPath) -> void {
    if (m_Logger)
        return;
    std::filesystem::create_directories(LogDirPath);
    auto FileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((LogDirPath / "SoulEngine.log").string(), true);
    m_EditorSink  = FileSink.get();
    auto ConsoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    m_ConsoleSink    = ConsoleSink.get();
    FileSink->set_formatter(CreateLogFormatter("[%m-%d %H:%M:%S.%e] [%=7l] %q %v", false));
    ConsoleSink->set_formatter(CreateLogFormatter("[%H:%M:%S.%e] [%^%=7l%$] %q %v", true));
    std::vector<SPtr<spdlog::sinks::sink>> Sinks;
    Sinks.emplace_back(FileSink);
    Sinks.emplace_back(ConsoleSink);
    m_Logger = std::make_unique<spdlog::logger>("SoulEngine", Sinks.begin(), Sinks.end());
    m_Logger->set_level(spdlog::level::trace);
    m_Logger->set_error_handler([](const String&) noexcept {});
}

auto LogManager::Log(LogLevel Level, StringView Message) -> void {
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

auto LogManager::SetSinkLevels(const std::optional<String>& FileLevel, const std::optional<String>& ConsoleLevel)
    -> void {
    ApplySinkLevel(m_Logger.get(), m_EditorSink, FileLevel ? StringView{*FileLevel} : StringView{}, LogLevel::Debug);
    ApplySinkLevel(
        m_Logger.get(), m_ConsoleSink, ConsoleLevel ? StringView{*ConsoleLevel} : StringView{}, LogLevel::Info);
}

auto LogManager::LogToFile(LogLevel Level, StringView Filename, StringView Message) -> void {
    static std::atomic<Uint32> FileCounter = 0;
    auto                       FileSink   = std::make_shared<spdlog::sinks::basic_file_sink_mt>(String{Filename}, true);
    auto                       TempLogger = std::make_shared<spdlog::logger>(
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

} // namespace SoulEngine
