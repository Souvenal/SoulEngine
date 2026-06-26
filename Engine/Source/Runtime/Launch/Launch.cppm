module;

#include <tracy/Tracy.hpp>

export module Launch;

import Core;
import Platform;
import Window;
import Application;
import RHI;
import Scene;
import Renderer;
import TaskGraph;
import Resource;
export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Launch {

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
    Scene::SceneSnapshot    SceneData;
    RHI::CommandList        CmdList;
};

constexpr Uint32 kSlotCount = 3;

class EngineLoop {
  public:
    [[nodiscard]] auto PreInit(std::span<char*> CmdLineArgs) -> std::expected<void, ErrorMessage> {
        auto EngineDir = Path(CmdLineArgs.front()).parent_path().parent_path();

        if (EngineDir.filename() != "Engine") {
            std::println(stderr, "FATAL: Invalid engine directory layout.");
            std::println(stderr, "  Expected root directory name: Engine");
            std::println(stderr, "  Resolved root:               {}", EngineDir.string());
            std::println(stderr, "  Binary path:                 {}", CmdLineArgs.front());
            std::exit(1);
        }

        ConfigManager::Get().Init(EngineDir);
        LogManager::Get().Init(ConfigManager::Get().LogsDirPath());

        auto LoadResult = ConfigManager::Get().LoadConfig();
        if (!LoadResult)
            return std::unexpected(LoadResult.error().Append("Failed to load config file"));

        auto& LogCfg = ConfigManager::Get().GetConfig().Log;
        LogManager::Get().SetSinkLevels(LogCfg.FileLevel, LogCfg.ConsoleLevel);

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

        if (auto R = RHI::RenderDevice::Create(WindowDisplay.GetNativeHandle()); !R) {
            Shutdown();
            return std::unexpected(R.error().Append("Failed to create RHI context"));
        }
        LogInfo("RHI context created successfully");

        // 3 reserved threads for Game/Render/RHI
        auto WorkerCount = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 3);
        m_TaskGraph.Init(WorkerCount);

        Resource::Manager::Get().Init(m_TaskGraph);

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

        Resource::Manager::Get().BeginShutdown();
        m_TaskGraph.Shutdown();
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

        // Release frame slot snapshots and command lists (SPtrs inside handles/variant commands)
        // before RenderDevice::Destroy tears down VMA.
        for (auto& Slot : m_Slots) {
            Slot.SceneData = {};
            Slot.CmdList = {};
        }

        // Release GPU textures before VMA allocator dies.
        Resource::Manager::Get().Clear();

        RHI::RenderDevice::Destroy();
        WindowDisplay.Shutdown();
    }

    [[nodiscard]] auto SwitchApplication(StringView Name) -> std::expected<void, ErrorMessage> {
        if (m_Application) {
            m_Application->OnDetach();
            m_Application.reset();
        }

        auto NewApp = Application::Application::Create(Name);
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
        m_TaskGraph.Shutdown();
        for (auto& Slot : m_Slots)
            Slot.Cv.notify_all();
    }

    // ── Loops ────────────────────────────────────────────────────────────────

    auto GameLoop() -> void {
        tracy::SetThreadName("GameLoop");
        SetLogThreadRole(LogThreadRole::Game);
        while (!m_FatalError.load(std::memory_order_acquire)) {
            if (WindowDisplay.PollEvents())
                break;

            auto Resize = WindowDisplay.ConsumeFramebufferResize();

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
                auto& Scene = m_Application->GetScene();
                const auto Width  = static_cast<Uint32>(std::max(0, Resize->Width));
                const auto Height = static_cast<Uint32>(std::max(0, Resize->Height));
                Scene.m_Camera.AllocateRenderTargets(Width, Height);
            }

            m_Application->OnTick(Delta);
            auto& AppScene = m_Application->GetScene();
            AppScene.UpdateTime();
            Slot.SceneData = AppScene.BuildSnapshot();

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

            for (std::size_t i = 0; i < SoulEngine::TaskGraph::kMaxTasksPerPoll; ++i) {
                auto Task = m_TaskGraph.TryDequeue(SoulEngine::ThreadQueue::Render);
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

            Slot.CmdList = std::move(*RenderResult);

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

            for (std::size_t i = 0; i < SoulEngine::TaskGraph::kMaxTasksPerPoll; ++i) {
                auto Task = m_TaskGraph.TryDequeue(SoulEngine::ThreadQueue::RHI);
                if (!Task)
                    break;
                (*Task)();
            }

            // Resource handles are passive state reads; publish completed sampled-texture uploads here
            // on the RHI thread before the next command list can observe them.
            Resource::Manager::Get().TickGpuPending();

            if (auto R = RHI::RenderDevice::Get().Execute(Slot.CmdList); !R) {
                LogError("RHI Execute fatal error:\n{}", R.error().ToString());
                SignalFatalError();
                break;
            }

            // Tracy docs: "put the FrameMark macro after you have completed
            // rendering the frame. Ideally, that would be right after the
            // swap buffers command." — Execute() does submit + present.
            FrameMark;

            {
                std::lock_guard Lock(Slot.Mutex);
                Slot.State = SlotState::RHIDone;
            }
            Slot.Cv.notify_all();

            m_RHISlotIndex = (m_RHISlotIndex + 1) % kSlotCount;
        }

        RHI::RenderDevice::Get().WaitIdle();
    }

    // ── State ───────────────────────────────────────────────────────────────

    WindowDisplay                         WindowDisplay;
    UPtr<Application::Application>        m_Application;
    std::chrono::steady_clock::time_point m_LastTickTime;

    SoulEngine::TaskGraph             m_TaskGraph;
    std::array<FrameSlot, kSlotCount> m_Slots = {};

    Uint32 m_GameSlotIndex   = 0;
    Uint32 m_RenderSlotIndex = 0;
    Uint32 m_RHISlotIndex    = 0;

    std::atomic<bool> m_FatalError = false;

    std::jthread m_RenderThread;
    std::jthread m_RHIThread;
};

} // namespace SoulEngine::Launch
