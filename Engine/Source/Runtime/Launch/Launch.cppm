module;

#include <tracy/Tracy.hpp>

export module Launch;

import Core;
import Platform;
import Window;
import Application;
import RHI;

export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Launch {

class EngineLoop {
  public:
    /// @brief Process command-line arguments and load engine configuration.
    /// @details Currently only loads the engine configuration file; no
    /// command-line argument handling is implemented yet.
    [[nodiscard]] auto PreInit(std::span<char*> CmdLineArgs) -> std::expected<void, ErrorMessage> {
        // Directory layout:
        //   Engine/
        //     Configs/
        //       SoulEngine.toml
        //     Binaries/
        //       SoulEngine         <- binary (CmdLineArgs[0])
        auto EngineDir = Path(CmdLineArgs.front()).parent_path().parent_path();

        // Validate the directory layout before proceeding.  If the binary
        // has been moved, the hard-coded parent_path chain yields garbage.
        // Logging isn't initialized yet, so we write directly to stderr.
        if (EngineDir.filename() != "Engine") {
            std::println(stderr, "FATAL: Invalid engine directory layout.");
            std::println(stderr, "  Expected root directory name: Engine");
            std::println(stderr, "  Resolved root:               {}", EngineDir.string());
            std::println(stderr, "  Binary path:                 {}", CmdLineArgs.front());
            std::println(stderr, "  Layout must be: Engine/Binaries/Bin/<executable>");
            // Exit directly: the logging system hasn't been initialized yet,
            // so returning an error would end up in LogError with no logger.
            // No resources have been allocated at this point, so exit is safe.
            std::exit(1);
        }

        // Initialize ConfigManager with the engine root — infallible, no I/O.
        ConfigManager::Get().Init(EngineDir);

        // Initialize spdlog sinks and logger rooted at Engine/Logs/.
        LogManager::Get().Init(ConfigManager::Get().LogsDirPath());

        // Load the config file from Engine/Configs/SoulEngine.toml.
        auto LoadResult = ConfigManager::Get().LoadConfig();
        if (!LoadResult)
            return std::unexpected(LoadResult.error().Append("Failed to load config file"));

        // Apply log-level configuration from the [Log] section.
        auto& LogCfg = ConfigManager::Get().GetConfig().Log;
        LogManager::Get().SetSinkLevels(LogCfg.FileLevel, LogCfg.ConsoleLevel);

        // LogInfo intentionally deferred to here: SLogManager::Init() is
        // called above and must run first so the spdlog logger is ready.
        // Any log calls before that point silently produce no output
        // (Log() returns early when the logger is null).
        LogInfo("Soul Engine PreInitializing... ({} args)", CmdLineArgs.size());

        Platform::InstallCrashHandler();

        return {};
    }

    [[nodiscard]] auto Init() -> std::expected<void, ErrorMessage> {
        LogInfo("Soul Engine Initializing...");

        auto WinResult = WindowDisplay::Create();
        if (!WinResult)
            return std::unexpected(WinResult.error().Append("Failed to create window display"));
        WindowDisplay = std::move(*WinResult);

        // ── RHI context — process-wide singleton ──────────────────────────
        if (auto R = RHI::RenderDevice::Create(WindowDisplay.GetNativeHandle()); !R) {
            Shutdown();
            return std::unexpected(R.error().Append("Failed to create RHI context"));
        }
        LogInfo("RHI context created successfully");

        // ── Create application from config ───────────────────────────────
        auto& Cfg = ConfigManager::Get().GetConfig();
        if (auto R = SwitchApplication(Cfg.Application.Name.value_or("Test")); !R) {
            Shutdown();
            return std::unexpected(R.error().Append("SwitchApplication failed"));
        }
        LogInfo("Application '{}' initialized successfully", Cfg.Application.Name.value_or("Test"));

        m_LastTickTime = std::chrono::steady_clock::now();
        bIsRunning     = true;
        return {};
    }

    auto Shutdown() -> void {
        LogInfo("Shutting down...");

        // GPU must finish all in-flight work before resources are destroyed.
        // Application resources (VertexBuffer, etc.) are freed when the app
        // resets; their DeviceBuffer destructors call vmaDestroyBuffer, which
        // fails if the GPU still references them.
        RHI::RenderDevice::Get().WaitIdle();

        if (m_Application) {
            m_Application->OnDetach();
            m_Application.reset();
        }

        RHI::RenderDevice::Destroy();
        WindowDisplay.Shutdown();
    }

    auto RequestExit() -> void {
        bIsRunning = false;
    }

    [[nodiscard]] auto IsExitRequested() -> bool {
        return !bIsRunning || WindowDisplay.IsExitRequested();
    }

    auto Tick() -> void {
        auto  Now       = std::chrono::steady_clock::now();
        float DeltaTime = std::chrono::duration<float>(Now - m_LastTickTime).count();
        m_LastTickTime  = Now;

        WindowDisplay.PollEvents();

        if (m_Application) {
            m_Application->OnTick(DeltaTime);
            m_Application->OnRender();
        }

        FrameMark;
    }

    [[nodiscard]] auto SwitchApplication(StringView Name) -> std::expected<void, ErrorMessage> {
        // Detach previous application (renderer shut down along with it)
        if (m_Application) {
            m_Application->OnDetach();
            m_Application.reset();
        }

        // Create and attach new application
        auto NewApp = Application::Application::Create(Name);
        if (!NewApp)
            return std::unexpected(NewApp.error().Append("SwitchApplication failed"));

        if (auto R = (*NewApp)->OnAttach(); !R)
            return std::unexpected(R.error().Append("Application OnAttach failed"));

        m_Application = std::move(*NewApp);
        return {};
    }

  private:
    bool                                  bIsRunning = false;
    WindowDisplay                         WindowDisplay;
    UPtr<Application::Application>        m_Application;
    std::chrono::steady_clock::time_point m_LastTickTime;
};

} // namespace SoulEngine::Launch
