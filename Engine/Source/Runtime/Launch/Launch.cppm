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
    SceneSnapshot           SceneData;
    SPtr<IRenderer>         Renderer = nullptr;
    RenderResult            RenderPacket;
    ImDrawDataSnapshot      ImGuiSnapshot;
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

        m_Editor.Initialize();

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

        if (auto R = m_Editor.BindPresentation(m_WindowSystem.get(), &RHIRenderDevice::Get()); !R) {
            Shutdown();
            return std::unexpected(R.error().Append("Editor presentation binding failed"));
        }

        // 3 reserved threads for Game/Render/RHI
        auto WorkerCount = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 3);
        TaskGraph::Get().Init(WorkerCount);

        ResourceManager::Get().Init();

        auto&      Cfg             = ConfigManager::Get().GetConfig();
        const auto InitialRenderer = Cfg.Render.DefaultRenderer.value_or("Raster");
        if (auto R = SelectRenderer(InitialRenderer); !R) {
            Shutdown();
            return std::unexpected(R.error().Append("Default renderer selection failed"));
        }
        LogInfo("Renderer '{}' initialized successfully", InitialRenderer);
        // ── Create application from config ───────────────────────────────
        if (auto R = OpenApplication(Cfg.Application.Name.value_or("Test")); !R) {
            Shutdown();
            return std::unexpected(R.error().Append("Application opening failed"));
        }
        auto* CurrentApplication = GetCurrentApplication();
        if (!CurrentApplication) {
            Shutdown();
            return std::unexpected(ErrorMessage("Application was not available after opening"));
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

        CloseApplication();

        // Release frame slot snapshots and command observers before
        // ResourceManager::Clear() and RenderDevice::Destroy() tear down VMA.
        for (auto& Slot : m_Slots) {
            Slot.SceneData    = {};
            Slot.Renderer     = nullptr;
            Slot.RenderPacket = {};
        }

        // Release editor GPU resources before ResourceManager::Clear() and
        // RenderDevice::Destroy() tear down the backend.
        m_Editor.ReleaseRHIResources();

        CloseRenderers();

        // Release GPU textures before VMA allocator dies.
        ResourceManager::Get().Clear();

        RHIRenderDevice::Destroy();
        if (m_WindowSystem) {
            m_WindowSystem->Shutdown();
            m_WindowSystem.reset();
        }
        m_Editor.Shutdown();
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
            if (m_WindowSystem->Tick())
                break;

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

            // Each engine loop owns an independent ordinal. Render/RHI tasks
            // carry the producer's ordinal and execute when the consumer
            // reaches the same pipeline position.
            TaskGraph::Get().IncreaseThreadFrameIndex();
            // UI builds on the main thread so ImGui input stays on the same
            // thread as event polling; the render thread consumes snapshots.
            m_Editor.BeginFrame(Slot.ImGuiSnapshot);

            auto* CurrentApplication = GetCurrentApplication();
            if (!CurrentApplication) {
                LogError("Game loop has no active application");
                SignalFatalError();
                break;
            }
            Slot.Renderer = GetCurrentRenderer();
            if (!Slot.Renderer) {
                LogError("Game loop has no active renderer");
                SignalFatalError();
                break;
            }
            auto& AppScene = CurrentApplication->GetScene();
            m_Editor.Tick(Delta);
            AppScene.UpdateTime();
            AppScene.Tick(Delta);
            if (auto SceneView = m_Editor.BuildSceneView()) {
                const std::array SceneViews{std::move(*SceneView)};
                const auto       PickSnapshot = AppScene.BuildSnapshot();
                m_Editor.UpdateSceneSelection(AppScene, SceneViews.front(), PickSnapshot);
                Slot.SceneData =
                    AppScene.BuildSnapshot(SceneViews, m_Editor.GetSelectedEntity(), m_Editor.GetSelectedPixel());
            } else {
                Slot.SceneData = AppScene.BuildSnapshot(m_Editor.GetSelectedEntity(), m_Editor.GetSelectedPixel());
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

            // Advance RenderThread's local frame ordinal before draining or
            // publishing tasks for this frame.
            TaskGraph::Get().IncreaseThreadFrameIndex();
            TaskGraph::Get().DrainTasks(ThreadQueue::Render);

            if (!Slot.Renderer) {
                LogError("Render loop received a frame without a renderer");
                SignalFatalError();
                break;
            }
            auto RenderResult = Slot.Renderer->Render(Slot.SceneData);
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

            // Advance RHIThread's local frame ordinal before matching its
            // frame-affined tasks. This intentionally does not read another
            // thread's TLS value.
            TaskGraph::Get().IncreaseThreadFrameIndex();
            TaskGraph::Get().DrainTasks(ThreadQueue::RHI);
            TaskGraph::Get().DrainFrameTasks(ThreadQueue::RHI);

            // Resource handles are passive state reads; publish completed sampled-texture uploads here
            // on the RHI thread before the next command list can observe them.
            RHIRenderDevice::Get().Tick();
            ResourceManager::Get().TickRhiDependencies();

            if (auto R = RHIRenderDevice::Get().Execute(std::move(Slot.RenderPacket.CmdList)); !R) {
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

    Editor                                m_Editor;
    UPtr<IWindowSystem>                   m_WindowSystem;
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
