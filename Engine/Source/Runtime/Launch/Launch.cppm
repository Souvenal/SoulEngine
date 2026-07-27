module;

#include <tracy/Tracy.hpp>

// Required while Scene exposes entt::registry in its object layout. EngineLoop owns
// Application and can instantiate the Scene lifetime chain when replacing applications.
#include <entt/entt.hpp>
#include <imgui_threaded_rendering.h>

export module Launch;

import Core;
import Platform;
import WindowSystem;
import Application;
import Editor;
import RHI;
import Scene;
import Renderer;
import TaskGraph;
import Resource;
export import std;

export namespace SoulEngine {

/// @brief Per-frame slot state in the triple-buffered Game→Render→RHI pipeline.
enum class SlotState {
    Unknown = 0,
    Empty,
    GameReady,
    RenderReady,
    RHIDone,
};

/// @brief One element of the triple buffer.
struct FrameSlot {
    std::mutex              Mutex;
    std::condition_variable Cv;
    SlotState               State = SlotState::Empty;
    SceneSnapshot    SceneData;
    RenderResult  RenderPacket;
    ImDrawDataSnapshot ImGuiSnapshot;
};

constexpr Uint32 kSlotCount = 3;

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

        // LogInfo intentionally deferred to here: LogManager::Init() is
        // called above and must run first so the spdlog logger is ready.
        // Any log calls before that point silently produce no output
        // (Log() returns early when the logger is null).
        LogInfo("Soul Engine PreInitializing... ({} args)", CmdLineArgs.size());
        Platform::InstallCrashHandler();
        return {};
    }

    [[nodiscard]] auto Init() -> std::expected<void, ErrorMessage> {
        LogInfo("Soul Engine Initializing...");

        if (auto R = m_Editor.Create(); !R) {
            Shutdown();
            return std::unexpected(R.error().Append("Editor creation failed"));
        }

        auto WinResult = CreateWindowSystem();
        if (!WinResult)
            return std::unexpected(WinResult.error().Append("Failed to create window system"));
        m_WindowSystem = std::move(*WinResult);

        // ── RHI context — process-wide singleton ──────────────────────────
        if (auto R = RHIRenderDevice::Create(m_WindowSystem.get()); !R) {
            Shutdown();
            return std::unexpected(R.error().Append("Failed to create RHI context"));
        }
        LogInfo("RHI context created successfully");

        // 3 reserved threads for Game/Render/RHI
        auto WorkerCount = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 3);
        TaskGraph::Get().Init(WorkerCount);

        ResourceManager::Get().Init();

        if (auto R = m_Editor.BindPresentation(m_WindowSystem.get(), &RHIRenderDevice::Get()); !R) {
            Shutdown();
            return std::unexpected(R.error().Append("Editor presentation binding failed"));
        }
        const auto InitialExtent = m_WindowSystem->GetFramebufferExtent();
        m_Editor.ResizeSceneViewport(
            static_cast<Uint32>(std::max(0, InitialExtent.Width)), static_cast<Uint32>(std::max(0, InitialExtent.Height)));

        // ── Create application from config ───────────────────────────────
        auto& Cfg = ConfigManager::Get().GetConfig();
        if (auto R = SwitchApplication(Cfg.Application.Name.value_or("Test")); !R) {
            Shutdown();
            return std::unexpected(R.error().Append("SwitchApplication failed"));
        }
        LogInfo("Application '{}' initialized successfully", Cfg.Application.Name.value_or("Test"));

        return {};
    }

    /// @brief Spawn worker threads and run the main-thread game loop.
    auto Run() -> void {
        LogInfo("Starting engine run loop...");

        m_LastTickTime = std::chrono::steady_clock::now();
        m_RenderThread = std::jthread{[this](std::stop_token S) { RenderLoop(S); }};
        m_RHIThread    = std::jthread{[this](std::stop_token S) { RHILoop(S); }};

        GameLoop();
        Shutdown();
    }

    auto Shutdown() -> void {
        LogInfo("Shutting down...");

        // Request stop BEFORE notifying CVs — otherwise threads wake,
        // check stop_requested() == false, go back to sleep, and never
        // wake again because request_stop() does not notify condition
        // variables.
        if (m_RenderThread.joinable())
            m_RenderThread.request_stop();
        if (m_RHIThread.joinable())
            m_RHIThread.request_stop();

        ResourceManager::Get().BeginShutdown();
        TaskGraph::Get().Shutdown();
        for (auto& Slot : m_Slots)
            Slot.Cv.notify_all();

        if (m_RenderThread.joinable())
            m_RenderThread.join();
        if (m_RHIThread.joinable())
            m_RHIThread.join();

        if (m_Application) {
            m_Application->OnDetach();
            m_Application.reset();
        }

        // Release frame slot snapshots and command observers before
        // ResourceManager::Clear() and RenderDevice::Destroy() tear down VMA.
        for (auto& Slot : m_Slots) {
            Slot.SceneData = {};
            Slot.RenderPacket = {};
        }

        // Release editor GPU resources before ResourceManager::Clear() and
        // RenderDevice::Destroy() tear down the backend.
        m_Editor.ReleaseRHIResources();

        // Release GPU textures before VMA allocator dies.
        ResourceManager::Get().Clear();

        RHIRenderDevice::Destroy();
        if (m_WindowSystem) {
            m_WindowSystem->Shutdown();
            m_WindowSystem.reset();
        }
        m_Editor.Shutdown();
    }

    [[nodiscard]] auto SwitchApplication(StringView Name) -> std::expected<void, ErrorMessage> {
        // Detach previous application (renderer shut down along with it)
        if (m_Application) {
            m_Application->OnDetach();
            m_Application.reset();
        }

        // Create and attach new application
        auto NewApp = Application::Create(Name);
        if (!NewApp)
            return std::unexpected(NewApp.error().Append("SwitchApplication failed"));

        if (auto R = (*NewApp)->OnAttach(); !R)
            return std::unexpected(R.error().Append("Application OnAttach failed"));

        m_Application = std::move(*NewApp);
        return {};
    }

  private:
    /// @brief Broadcast fatal error to all loops and trigger teardown.
    auto SignalFatalError() -> void {
        m_FatalError.store(true, std::memory_order_release);
        TaskGraph::Get().Shutdown();
        for (auto& Slot : m_Slots)
            Slot.Cv.notify_all();
    }

    // ── Loops ────────────────────────────────────────────────────────────────

    auto GameLoop() -> void {
        tracy::SetThreadName("GameLoop");
        SetLogThreadRole(LogThreadRole::Game);
        while (!m_FatalError.load(std::memory_order_acquire)) {
            if (m_WindowSystem->PollEvents())
                break;

            auto Resize = m_WindowSystem->ConsumeFramebufferResize();

            auto  Now      = std::chrono::steady_clock::now();
            float Delta    = std::chrono::duration<float>(Now - m_LastTickTime).count();
            m_LastTickTime = Now;

            auto& Slot = m_Slots[m_GameSlotIndex];

            {
                std::unique_lock Lock(Slot.Mutex);
                Slot.Cv.wait(Lock, [this, &Slot] {
                    return Slot.State == SlotState::Empty || Slot.State == SlotState::RHIDone ||
                           m_FatalError.load(std::memory_order_relaxed);
                });
            }
            if (m_FatalError.load(std::memory_order_acquire))
                break;

            if (Resize) {
                const auto Width  = static_cast<Uint32>(std::max(0, Resize->Width));
                const auto Height = static_cast<Uint32>(std::max(0, Resize->Height));
                m_Editor.ResizeSceneViewport(Width, Height);
            }

            m_Application->OnTick(Delta, *m_WindowSystem);
            // UI builds on the main thread so ImGui input stays on the same
            // thread as event polling; the render thread consumes snapshots.
            m_Editor.BeginFrame(Slot.ImGuiSnapshot);
            m_Editor.UpdateSceneCamera(Delta, *m_WindowSystem);
            auto& AppScene = m_Application->GetScene();
            AppScene.UpdateTime();
            if (auto SceneView = m_Editor.BuildSceneView()) {
                const std::array Views{std::move(*SceneView)};
                Slot.SceneData = AppScene.BuildSnapshot(Views);
            } else {
                Slot.SceneData = AppScene.BuildSnapshot();
            }

            {
                std::lock_guard Lock(Slot.Mutex);
                Slot.State = SlotState::GameReady;
            }
            Slot.Cv.notify_all();

            m_GameSlotIndex = (m_GameSlotIndex + 1) % kSlotCount;
        }
    }

    auto RenderLoop(std::stop_token Stop) -> void {
        tracy::SetThreadName("RenderLoop");
        SetLogThreadRole(LogThreadRole::Render);
        while (!Stop.stop_requested()) {
            auto& Slot = m_Slots[m_RenderSlotIndex];

            {
                std::unique_lock Lock(Slot.Mutex);
                Slot.Cv.wait(Lock, [&] { return Slot.State == SlotState::GameReady || Stop.stop_requested(); });
            }
            if (Stop.stop_requested())
                break;

            for (std::size_t i = 0; i < TaskGraph::kMaxTasksPerPoll; ++i) {
                auto Task = TaskGraph::Get().TryDequeue(ThreadQueue::Render);
                if (!Task)
                    break;
                (*Task)();
            }

            auto RenderResult = m_Application->GetRenderer().Render(Slot.SceneData);
            if (!RenderResult) {
                LogError("Render fatal error:\n{}", RenderResult.error().ToString());
                SignalFatalError();
                break;
            }

            // Editor UI overlays the scene output on the same render thread.
            m_Editor.AttachPresentationOverlay(RenderResult->CmdList, Slot.ImGuiSnapshot);

            Slot.RenderPacket = std::move(*RenderResult);

            {
                std::lock_guard Lock(Slot.Mutex);
                Slot.State = SlotState::RenderReady;
            }
            Slot.Cv.notify_all();

            m_RenderSlotIndex = (m_RenderSlotIndex + 1) % kSlotCount;
        }
    }

    auto RHILoop(std::stop_token Stop) -> void {
        tracy::SetThreadName("RHILoop");
        SetLogThreadRole(LogThreadRole::RHI);

        while (!Stop.stop_requested()) {
            auto& Slot = m_Slots[m_RHISlotIndex];

            {
                std::unique_lock Lock(Slot.Mutex);
                Slot.Cv.wait(Lock, [&] { return Slot.State == SlotState::RenderReady || Stop.stop_requested(); });
            }
            if (Stop.stop_requested())
                break;

            for (std::size_t i = 0; i < TaskGraph::kMaxTasksPerPoll; ++i) {
                auto Task = TaskGraph::Get().TryDequeue(ThreadQueue::RHI);
                if (!Task)
                    break;
                (*Task)();
            }

            // Resource handles are passive state reads; publish completed sampled-texture uploads here
            // on the RHI thread before the next command list can observe them.
            ResourceManager::Get().TickGpuPending();

            if (auto R = RHIRenderDevice::Get().Execute(Slot.RenderPacket.CmdList); !R) {
                LogError("RHI Execute fatal error:\n{}", R.error().ToString());
                SignalFatalError();
                break;
            }

            // Tracy docs: "put the FrameMark macro after you have completed
            // rendering the frame. Ideally, that would be right after the
            // swap buffers command." — Execute() does submit + present.
            FrameMark;

            Slot.RenderPacket = {};
            ResourceManager::Get().CollectReleasedResources();

            {
                std::lock_guard Lock(Slot.Mutex);
                Slot.State = SlotState::RHIDone;
            }
            Slot.Cv.notify_all();

            m_RHISlotIndex = (m_RHISlotIndex + 1) % kSlotCount;
        }

        // GPU must finish all in-flight work before resources are destroyed.
        // Application resources (VertexBuffer, etc.) are freed when the app
        // resets; their DeviceBuffer destructors call vmaDestroyBuffer, which
        // fails if the GPU still references them.
        RHIRenderDevice::Get().WaitIdle();
    }

    // ── State ───────────────────────────────────────────────────────────────

    Editor                   m_Editor;
    UPtr<IWindowSystem>      m_WindowSystem;
    UPtr<Application>        m_Application;
    std::chrono::steady_clock::time_point m_LastTickTime;

    std::array<FrameSlot, kSlotCount> m_Slots = {};

    Uint32 m_GameSlotIndex   = 0;
    Uint32 m_RenderSlotIndex = 0;
    Uint32 m_RHISlotIndex    = 0;

    std::atomic<bool> m_FatalError = false;

    std::jthread m_RenderThread;
    std::jthread m_RHIThread;
};

} // namespace SoulEngine
