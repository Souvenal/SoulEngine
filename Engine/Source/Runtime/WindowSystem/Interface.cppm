module;

#include <entt/entt.hpp>

export module WindowSystem:Interface;

import :Types;
export import std;

export namespace SoulEngine {

/// @brief Window-system (WIS) abstraction: window lifecycle, event polling,
/// and per-frame input event publication.
///
/// All methods must be called from the engine main thread. Input state
/// is updated by platform callbacks and published through the window event
/// dispatcher once per frame.
///
/// Implementations are concrete window backends (GLFW today; Cocoa, WinUI,
/// etc. later). Backend adapters that need native objects and the ImGui GLFW
/// backend dispatch on GetType() and static_cast to the concrete class.
class IWindowSystem {
  public:
    IWindowSystem()          = default;
    virtual ~IWindowSystem() = default;

    IWindowSystem(const IWindowSystem&)                    = delete;
    auto operator=(const IWindowSystem&) -> IWindowSystem& = delete;
    IWindowSystem(IWindowSystem&&)                         = delete;
    auto operator=(IWindowSystem&&) -> IWindowSystem&      = delete;

    /// @brief Concrete implementation tag for RTTI-free downcasts.
    [[nodiscard]] virtual auto GetType() const -> WindowSystemType = 0;

    [[nodiscard]] virtual auto IsValid() const -> bool = 0;

    virtual auto Shutdown() -> void = 0;

    /// @brief Advance one window-system frame and publish input events.
    ///
    /// Implementations pump their platform events, update backend-specific
    /// input state, publish the frame snapshots through the dispatcher, and
    /// return true when a close was requested.
    [[nodiscard]] virtual auto Tick() -> bool = 0;

    /// @brief Return the dispatcher for window-system events.
    [[nodiscard]] virtual auto GetEventDispatcher() -> entt::dispatcher& = 0;

    /// @brief Return the current drawable framebuffer size in physical pixels.
    [[nodiscard]] virtual auto GetFramebufferExtent() const -> FramebufferExtent = 0;

    /// @brief Set the cursor presentation and confinement mode.
    virtual auto SetCursorMode(CursorMode Mode) -> void = 0;

  protected:
    KeyboardFrameEvent m_KeyboardFrameEvent = {};
    MouseFrameEvent    m_MouseFrameEvent    = {};
    entt::dispatcher   m_EventDispatcher    = {};
};

} // namespace SoulEngine
