module;

#include <tracy/Tracy.hpp>

// Required while Scene exposes entt::registry in its object layout. EngineLoop owns Application and can
// instantiate the Scene lifetime chain when replacing applications.
#include <entt/entt.hpp>
#include <imgui_threaded_rendering.h>

module Launch:Implementation.impl;

import Launch;
import Core;
import Platform;
import WindowSystem;
import Application;
import Editor;
import RHI;
import RenderGraph;
import Scene;
import Renderer;
import TaskGraph;
import std;
import EditorTypes;

namespace SoulEngine {

// Tracy identifies instrumented strings by pointer address, not content (see Tracy manual, "Unique
// pointers"): identical literals are not guaranteed to pool to one address, and a mismatched pointer silently
// splits frame sources. Always pass these named constants instead of fresh literals.
inline constexpr const char* kGameThreadName   = "GameThread";
inline constexpr const char* kRenderThreadName = "RenderThread";
inline constexpr const char* kRHIThreadName    = "RHIThread";
inline constexpr const char* kGameFrameName    = "GameFrame";
inline constexpr const char* kRenderFrameName  = "RenderFrame";
inline constexpr const char* kRHIFrameName     = "RHIFrame";

// Boot-time Launch CVars register during module initialization and configure startup.
CVar<String> CVarApplicationName{"Application.Name", "Test", "Application opened at startup"};
CVar<String> CVarRenderRHI{"Render.RHI", "Vulkan", "RHI backend created at startup"};

auto EngineLoop::PreInit(std::span<char*> CmdLineArgs) -> std::expected<void, ErrorMessage> {
    // Directory layout:
    //   Engine/
    //     Configs/
    //       SoulEngine.toml
    //     Binaries/
    //       SoulEngine         <- binary (CmdLineArgs[0])
    auto EngineDir = Path(CmdLineArgs.front()).parent_path().parent_path();

    // Validate the directory layout before proceeding. If the binary has been moved, the hard-coded
    // parent_path chain yields garbage. Logging isn't initialized yet, so we write directly to stderr.
    if (EngineDir.filename() != "Engine") {
        std::println(stderr, "FATAL: Invalid engine directory layout.");
        std::println(stderr, "  Expected root directory name: Engine");
        std::println(stderr, "  Resolved root:               {}", EngineDir.string());
        std::println(stderr, "  Binary path:                 {}", CmdLineArgs.front());
        // Exit directly: the logging system hasn't been initialized yet, so returning an error would end
        // up in LogError with no logger. No resources have been allocated at this point, so exit is safe.
        std::exit(1);
    }

    // Initialize EnginePaths with the engine root — infallible, no I/O.
    EnginePaths::Get().Init(EngineDir);

    // Initialize spdlog sinks and logger rooted at Engine/Logs/.
    LogManager::Get().Init(EnginePaths::Get().LogsDirPath());

    // Load the config file from Engine/Configs/SoulEngine.toml into the registered CVars (also flushes
    // static-init registration diagnostics).
    auto LoadResult = CVarManager::Get().LoadConfig(EnginePaths::Get().ConfigsDirPath() / "SoulEngine.toml");
    if (!LoadResult)
        return std::unexpected(LoadResult.error().Append("Failed to load config file"));

    // Apply log-level configuration from the [Log] section.
    LogManager::Get().SetSinkLevels();

    // LogInfo intentionally deferred to here: LogManager::Init() is called above and must run first so
    // the spdlog logger is ready. Any log calls before that point silently produce no output (Log()
    // returns early when the logger is null).
    LogInfo("Soul Engine PreInitializing... ({} args)", CmdLineArgs.size());
    InstallCrashHandler();
    return {};
}

auto EngineLoop::Init() -> std::expected<void, ErrorMessage> {
    LogInfo("Soul Engine Initializing...");

    m_Editor.Initialize();

    auto WinResult = CreateWindowSystem();
    if (!WinResult)
        return std::unexpected(WinResult.error().Append("Failed to create window system"));
    m_WindowSystem = std::move(*WinResult);
    // RHI context — process-wide singleton

    const auto Backend = CVarRenderRHI.Get();
    if (auto R = RHIRenderDevice::Create(Backend, m_WindowSystem.get()); !R) {
        Shutdown();
        return std::unexpected(R.error().Append("Failed to create RHI context"));
    }
    LogInfo("RHI context created successfully");

    // TaskGraph must be running before Editor binds its presentation: the editor world creates RHI
    // resources through the queued RHI path.
    auto WorkerCount = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 3);
    TaskGraph::Get().Init(WorkerCount);

    if (auto R = m_Editor.BindPresentation(m_WindowSystem.get(), &RHIRenderDevice::Get()); !R) {
        Shutdown();
        return std::unexpected(R.error().Append("Editor presentation binding failed"));
    }

    // Cross-frame render-graph facilities (pipeline registry + transient render-target pool)
    RenderGraph::Get().Init();

    const auto InitialRenderer = CVarRenderDefaultRenderer.Get();
    if (auto R = SelectRenderer(InitialRenderer); !R) {
        Shutdown();
        return std::unexpected(R.error().Append("Default renderer selection failed"));
    }
    // Create application from config
    LogInfo("Renderer '{}' initialized successfully", InitialRenderer);
    if (auto R = OpenApplication(CVarApplicationName.Get()); !R) {
        Shutdown();
        return std::unexpected(R.error().Append("Application opening failed"));
    }
    auto* CurrentApplication = GetCurrentApplication();
    if (!CurrentApplication) {
        Shutdown();
        return std::unexpected(ErrorMessage("Application was not available after opening"));
    }
    m_Editor.BindScene(CurrentApplication->GetScene());
    LogInfo("Application '{}' initialized successfully", CVarApplicationName.Get());

    return {};
}

auto EngineLoop::Run() -> void {
    LogInfo("Starting engine run loop...");

    m_LastTickTime = std::chrono::steady_clock::now();
    m_RenderThread = std::jthread{[this](std::stop_token S) { RenderLoop(S); }};
    m_RHIThread    = std::jthread{[this](std::stop_token S) { RHILoop(S); }};

    GameLoop();
    Shutdown();
}

auto EngineLoop::Shutdown() -> void {
    LogInfo("Shutting down...");

    // Request stop BEFORE notifying CVs — otherwise threads wake, check stop_requested() == false, go
    // back to sleep, and never wake again because request_stop() does not notify condition variables.
    if (m_RenderThread.joinable())
        m_RenderThread.request_stop();
    if (m_RHIThread.joinable())
        m_RHIThread.request_stop();

    for (auto& Slot : m_Slots)
        Slot.Cv.notify_all();

    // Join the workers before stopping TaskGraph: a worker caught mid-frame finishes its in-flight slot
    // against live task services instead of failing with spurious "TaskGraph is not running" errors
    // during an otherwise clean shutdown.
    if (m_RenderThread.joinable())
        m_RenderThread.join();
    if (m_RHIThread.joinable())
        m_RHIThread.join();

    TaskGraph::Get().Shutdown();

    m_Editor.UnbindScene();
    CloseApplication();

    // Release frame slot snapshots and command observers before RenderDevice::Destroy() tears down VMA.
    for (auto& Slot : m_Slots) {
        Slot.GameData     = {};
        Slot.EditorData   = {};
        Slot.Renderer     = nullptr;
        Slot.RenderPacket = {};
    }

    // Release editor GPU resources before RenderDevice::Destroy() tears down the backend.
    m_Editor.ReleaseRHIResources();

    CloseRenderers();

    // Release cross-frame render-graph state (pool + pipeline refs) while the deferred-deletion queue and
    // native device are still alive — MUST precede RHIRenderDevice::Destroy()
    RenderGraph::Get().Shutdown();

    if (auto R = RHIRenderDevice::Destroy(); !R)
        LogError("RHI teardown failed:\n{}", R.error().ToString());
    if (m_WindowSystem) {
        m_WindowSystem->Shutdown();
        m_WindowSystem.reset();
    }
    m_Editor.Shutdown();
}

auto EngineLoop::SignalFatalError() -> void {
    m_FatalError.store(true, std::memory_order_release);
    TaskGraph::Get().Shutdown();
    for (auto& Slot : m_Slots)
        Slot.Cv.notify_all();
}

auto EngineLoop::GameLoop() -> void {
    tracy::SetThreadName(kGameThreadName);
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
                // RenderReady means RenderThread consumed the previous SceneData; GameThread may prepare
                // the next snapshot even while the old RenderPacket remains GPU-owned.
                return Slot.State == SlotState::Empty || Slot.State == SlotState::RenderReady ||
                       m_FatalError.load(std::memory_order_relaxed);
            });
        }
        if (m_FatalError.load(std::memory_order_acquire))
            break;

        FrameMarkStart(kGameFrameName);

        // Each engine loop owns an independent ordinal. Render/RHI tasks carry the producer's ordinal and
        // execute when the consumer reaches the same pipeline position.
        TaskGraph::Get().IncreaseThreadFrameIndex();

        // UI builds on the main thread so ImGui input stays on the same thread as event polling; the
        // render thread consumes snapshots.
        //
        // Every published render packet owns its snapshot until GPU completion, so the next game tick
        // must render into a fresh one.
        Slot.ImGuiSnapshot = std::make_unique<ImDrawDataSnapshot>();
        m_Editor.Tick(Delta, *Slot.ImGuiSnapshot);

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
        AppScene.UpdateTime();
        AppScene.Tick(Delta);
        Slot.GameData   = AppScene.BuildSnapshot();
        Slot.EditorData = m_Editor.BuildSnapshot();

        {
            std::lock_guard Lock(Slot.Mutex);
            Slot.State = SlotState::GameReady;
        }
        Slot.Cv.notify_all();

        m_GameSlotIndex = (m_GameSlotIndex + 1) % kSlotCount;
        FrameMarkEnd(kGameFrameName);
    }
}

auto EngineLoop::RenderLoop(std::stop_token Stop) -> void {
    tracy::SetThreadName(kRenderThreadName);
    SetLogThreadRole(LogThreadRole::Render);
    while (!Stop.stop_requested()) {
        auto& Slot = m_Slots[m_RenderSlotIndex];

        {
            std::unique_lock Lock(Slot.Mutex);
            Slot.Cv.wait(Lock, [&] { return Slot.State == SlotState::GameReady || Stop.stop_requested(); });
        }
        if (Stop.stop_requested())
            break;

        FrameMarkStart(kRenderFrameName);

        // Advance RenderThread's local frame ordinal before draining or publishing tasks for this frame.
        TaskGraph::Get().IncreaseThreadFrameIndex();
        TaskGraph::Get().DrainTasks(ThreadQueue::Render);

        // Pool maintenance (LRU eviction) runs before this frame's Compile acquires.
        RenderGraph::Get().Tick();

        if (!Slot.Renderer) {
            LogError("Render loop received a frame without a renderer");
            SignalFatalError();
            break;
        }
        auto RenderResult = Slot.Renderer->Render(Slot.GameData, Slot.EditorData);
        if (!RenderResult) {
            LogError("Render fatal error:\n{}", RenderResult.error().ToString());
            SignalFatalError();
            break;
        }

        // Move the UI snapshot into the pending render result before publishing RenderReady. The shared
        // object remains alive through GPU completion even if this slot is reused immediately.
        RenderResult->ImGuiSnapshot = std::move(Slot.ImGuiSnapshot);
        Slot.ImGuiSnapshot.reset();

        {
            std::lock_guard Lock(Slot.Mutex);
            // RenderThread has finished reading SceneData. Publish this handoff before waiting on the old
            // packet so GameThread can prepare the next SceneData independently.
            Slot.State = SlotState::RenderReady;
        }
        Slot.Cv.notify_all();

        RHIFrameCompletion PreviousCompletion = {};
        {
            std::unique_lock Lock(Slot.Mutex);
            Slot.Cv.wait(Lock, [this, &Slot, &Stop] {
                // RHIThread must finish its CPU-side read before this thread overwrites the slot-owned
                // RenderPacket.
                return Slot.RhiConsumedRenderPacket || Stop.stop_requested() ||
                       m_FatalError.load(std::memory_order_relaxed);
            });
            if (Stop.stop_requested() || m_FatalError.load(std::memory_order_acquire))
                break;
            PreviousCompletion = Slot.Completion;
        }

        // This is the exact replacement boundary: CPU-side RHI access has ended above, and this wait
        // proves the GPU no longer executes the old packet before its RHIRefs are released by replacement.
        if (auto R = RHIRenderDevice::Get().WaitFinish(PreviousCompletion); !R) {
            LogError("Render loop GPU completion wait failed:\n{}", R.error().ToString());
            SignalFatalError();
            break;
        }

        {
            std::lock_guard Lock(Slot.Mutex);
            Slot.RenderPacket = std::move(*RenderResult);
            // Attach after the move so the borrowed pointer names the packet-owned snapshot retained
            // through GPU completion.
            if (Slot.RenderPacket.ImGuiSnapshot)
                m_Editor.AttachPresentationOverlay(Slot.RenderPacket.CmdList, *Slot.RenderPacket.ImGuiSnapshot);
            // The newly published packet is now pending RHIThread consumption. RHIThread waits on this
            // false value.
            Slot.RhiConsumedRenderPacket = false;
        }
        // Wake RHIThread after the complete packet and its RHIRefs are visible under the slot mutex.
        Slot.Cv.notify_all();

        m_RenderSlotIndex = (m_RenderSlotIndex + 1) % kSlotCount;
        FrameMarkEnd(kRenderFrameName);
    }
}

auto EngineLoop::RHILoop(std::stop_token Stop) -> void {
    tracy::SetThreadName(kRHIThreadName);
    SetLogThreadRole(LogThreadRole::RHI);

    while (!Stop.stop_requested()) {
        auto& Slot = m_Slots[m_RHISlotIndex];

        {
            std::unique_lock Lock(Slot.Mutex);
            Slot.Cv.wait(Lock, [&] {
                // RenderThread clears this flag only after waiting for the old GPU work and publishing a
                // complete new packet. RHIThread can therefore consume the packet without racing
                // RenderThread's replacement.
                return !Slot.RhiConsumedRenderPacket || Stop.stop_requested() ||
                       m_FatalError.load(std::memory_order_relaxed);
            });
        }
        if (Stop.stop_requested() || m_FatalError.load(std::memory_order_acquire))
            break;

        FrameMarkStart(kRHIFrameName);

        if (auto R = RHIRenderDevice::Get().BeginFrame(); !R) {
            LogError("RHI BeginFrame fatal error:\n{}", R.error().ToString());
            SignalFatalError();
            break;
        }

        // Advance RHIThread's local frame ordinal before matching its frame-affined tasks. This
        // intentionally does not read another thread's TLS value.
        TaskGraph::Get().IncreaseThreadFrameIndex();
        TaskGraph::Get().DrainTasks(ThreadQueue::RHI);
        TaskGraph::Get().DrainFrameTasks(ThreadQueue::RHI);

        // Publish completed sampled-texture uploads here on the RHI thread before the next command list
        // can observe them.
        if (auto R = RHIRenderDevice::Get().Tick(); !R) {
            LogError("RHI Tick fatal error:\n{}", R.error().ToString());
            SignalFatalError();
            break;
        }
        // Retire native resources whose last RHIRef was released since the previous frame.
        DrainRHIDeferredDeletions();

        // Execute borrows the slot-owned packet. RhiConsumedRenderPacket remains false while this call
        // and EndFrame() read it.
        if (auto R = RHIRenderDevice::Get().Execute(Slot.RenderPacket); !R) {
            LogError("RHI Execute fatal error:\n{}", R.error().ToString());
            SignalFatalError();
            break;
        }

        auto CompletionResult = RHIRenderDevice::Get().EndFrame();
        if (!CompletionResult) {
            LogError("RHI EndFrame fatal error:\n{}", CompletionResult.error().ToString());
            SignalFatalError();
            break;
        }

        {
            std::lock_guard Lock(Slot.Mutex);
            Slot.Completion              = *CompletionResult;
            // RHIThread is done with all CPU-side packet reads. GPU completion is tracked separately by
            // the immutable token.
            Slot.RhiConsumedRenderPacket = true;
        }
        // RenderThread may be waiting to replace this packet; notify it even when the GPU timeline has
        // already completed the work.
        Slot.Cv.notify_all();

        m_RHISlotIndex = (m_RHISlotIndex + 1) % kSlotCount;
        FrameMarkEnd(kRHIFrameName);

        // Unnamed master frame mark: feeds the GUI's default numbered frame set (framerate, frame
        // images). The named continuous spans above carry the per-stage intervals.
        FrameMark;
    }

    // GPU must finish all in-flight work before resources are destroyed. Application resources
    // (VertexBuffer, etc.) are freed when the app resets; their DeviceBuffer destructors call
    // vmaDestroyBuffer, which fails if the GPU still references them.
    if (auto R = RHIRenderDevice::Get().WaitIdle(); !R) {
        LogError("RHI WaitIdle fatal error:\n{}", R.error().ToString());
        SignalFatalError();
    }
}

} // namespace SoulEngine
