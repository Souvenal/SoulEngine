export module WindowSystem:Interface;

import :Types;
export import std;

export namespace SoulEngine {

/// @brief Window-system (WIS) abstraction: window lifecycle, event polling,
/// and consume-style input queries.
///
/// All methods must be called from the engine main thread. Input state
/// (scroll / cursor deltas) is accumulated by platform callbacks and consumed
/// once per game tick.
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

    /// @brief Pump platform events. Returns true when a close was requested.
    [[nodiscard]] virtual auto PollEvents() -> bool = 0;

    [[nodiscard]] virtual auto ConsumeFramebufferResize() -> std::optional<FramebufferExtent> = 0;

    /// @brief Return the current drawable framebuffer size in physical pixels.
    [[nodiscard]] virtual auto GetFramebufferExtent() const -> FramebufferExtent = 0;

    /// @brief Return whether a supported keyboard key is currently pressed.
    [[nodiscard]] virtual auto IsKeyPressed(WindowKey Key) const -> bool = 0;

    /// @brief Return whether a supported mouse button is currently pressed.
    [[nodiscard]] virtual auto IsMouseButtonPressed(WindowMouseButton Button) const -> bool = 0;

    /// @brief Show or lock the cursor for direct camera-style input.
    virtual auto SetCursorCaptured(bool Captured) -> void = 0;

    /// @brief Return and clear the vertical scroll amount received since the last call.
    virtual auto ConsumeScrollDelta() -> float = 0;

    /// @brief Return and clear the relative cursor movement received since the last call.
    virtual auto ConsumeCursorDelta() -> CursorDelta = 0;
};

} // namespace SoulEngine
