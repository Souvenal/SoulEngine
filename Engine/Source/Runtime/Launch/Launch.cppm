module;

// Required while Scene exposes entt::registry in its object layout. EngineLoop owns Application and can
// instantiate the Scene lifetime chain when replacing applications.
#include <entt/entt.hpp>
#include <imgui_threaded_rendering.h>

export module Launch;

import Core;
import WindowSystem;
import Editor;
import RHI;
import Scene;
import Renderer;
import std;
import EditorTypes;

namespace SoulEngine {

/// @brief Per-frame slot state in the triple-buffered Game→Render→RHI pipeline.
export enum class SlotState {
    Unknown = 0,
    Empty,
    GameReady,
    RenderReady,
};

/// @brief One element of the triple buffer.
export struct FrameSlot {
    std::mutex               Mutex;
    std::condition_variable  Cv;
    SlotState                State = SlotState::Empty;
    GameSnapshot             GameData;
    EditorSnapshot           EditorData;
    SPtr<IRenderer>          Renderer = nullptr;
    RenderResult             RenderPacket;
    UPtr<ImDrawDataSnapshot> ImGuiSnapshot           = nullptr;
    // Zero means this slot has not submitted GPU work yet, so its first RenderPacket does not need a timeline
    // wait before replacement.
    RHIFrameCompletion       Completion              = {};
    // RHIThread flips this false only while it may still read RenderPacket. RenderThread must observe true
    // before replacing that packet.
    bool                     RhiConsumedRenderPacket = true;
};

export constexpr Uint32 kSlotCount = 3;

// Tracy identifies instrumented strings by pointer address, not content (see Tracy manual, "Unique
// pointers"): identical literals are not guaranteed to pool to one address, and a mismatched pointer silently
// splits frame sources. Always pass these named constants instead of fresh literals.
export inline constexpr const char* kGameThreadName   = "GameThread";
export inline constexpr const char* kRenderThreadName = "RenderThread";
export inline constexpr const char* kRHIThreadName    = "RHIThread";
export inline constexpr const char* kGameFrameName    = "GameFrame";
export inline constexpr const char* kRenderFrameName  = "RenderFrame";
export inline constexpr const char* kRHIFrameName     = "RHIFrame";

export class EngineLoop {
  public:
    /// @brief Process command-line arguments and load engine configuration.
    /// @details Currently only loads the engine configuration file; no
    /// command-line argument handling is implemented yet.
    [[nodiscard]] auto PreInit(std::span<char*> CmdLineArgs) -> std::expected<void, ErrorMessage>;

    [[nodiscard]] auto Init() -> std::expected<void, ErrorMessage>;

    /// @brief Spawn worker threads and run the main-thread game loop.
    auto Run() -> void;

    auto Shutdown() -> void;

  private:
    /// @brief Broadcast fatal error to all loops and trigger teardown.
    auto SignalFatalError() -> void;
    // Loops

    auto GameLoop() -> void;

    auto RenderLoop(std::stop_token Stop) -> void;

    auto RHILoop(std::stop_token Stop) -> void;
    // State

    Editor                                m_Editor;
    UPtr<IWindowSystem>                   m_WindowSystem;
    std::chrono::steady_clock::time_point m_LastTickTime;

    std::array<FrameSlot, kSlotCount> m_Slots           = {};
    Uint32                            m_GameSlotIndex   = 0;
    Uint32                            m_RenderSlotIndex = 0;
    Uint32                            m_RHISlotIndex    = 0;

    std::atomic<bool> m_FatalError = false;

    std::jthread m_RenderThread;
    std::jthread m_RHIThread;
};

} // namespace SoulEngine
