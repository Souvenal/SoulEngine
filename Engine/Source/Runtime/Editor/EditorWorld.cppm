module;

#include <entt/entt.hpp>

export module Editor.EditorWorld;

import std;
import Core;
import EditorTypes;
import WindowSystem;

namespace SoulEngine {

/// @brief ECS world owned by the editor.
///
/// Contains editor-only entities, systems, and camera render resources.
export class EditorWorld {
  public:
    EditorWorld();

    /// @brief Initialize editor ECS entities and systems after RHI setup.
    auto Initialize(IWindowSystem& Window) -> void;

    ~EditorWorld();

    EditorWorld(const EditorWorld&)                    = delete;
    auto operator=(const EditorWorld&) -> EditorWorld& = delete;
    EditorWorld(EditorWorld&&)                         = delete;
    auto operator=(EditorWorld&&) -> EditorWorld&      = delete;

    /// @brief Subscribe the viewport camera to window framebuffer events.
    auto BindWindowEvents(IWindowSystem& Window) -> void;

    /// @brief Disconnect from window framebuffer events.
    auto UnbindWindowEvents() -> void;

    /// @brief Update editor ECS systems for one frame.
    auto Tick(Float32 DeltaTime) -> void;

    /// @brief Build the editor-owned Scene View render request for this frame.
    [[nodiscard]] auto BuildSnapshot() -> EditorSnapshot;

    /// @brief Shut down editor ECS systems and release their owned resources.
    auto Shutdown() -> void;

  private:
    auto OnFramebufferResize(FramebufferResizeEvent& Event) -> void;

    entt::registry    m_Registry = {};
    SystemScheduler   m_SystemScheduler;
    entt::dispatcher* m_WindowEventDispatcher = nullptr;
    entt::entity      m_EditorRootEntity      = entt::null;
};

} // namespace SoulEngine
