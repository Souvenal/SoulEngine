module;

#include <entt/entt.hpp>

export module WindowSystem.Interface;

import WindowSystem.Types;
import WindowSystem.Input;
import EditorCommands;
import Core;
import std;

namespace SoulEngine {

export CVar<Int32> CVarWindowResolutionX{"Window.ResolutionX", 1280, "Window width in logical pixels"};
export CVar<Int32> CVarWindowResolutionY{"Window.ResolutionY", 720, "Window height in logical pixels"};

/// @brief Window-system (WIS) abstraction: window lifecycle, event polling,
/// and per-frame input event publication.
///
/// All methods must be called on Game. Platform callbacks submit input commands, and
/// AdvanceFrame publishes executed input through the window event dispatcher once per frame.
///
/// Implementations are concrete window backends (GLFW today; Cocoa, WinUI, etc. later). Backend adapters that
/// need native objects and the ImGui GLFW backend dispatch on GetType() and static_cast to the concrete
/// class.
export class IWindowSystem {
  public:
    IWindowSystem() : m_InputCommands(m_InputFrameState) {}
    virtual ~IWindowSystem() = default;

    IWindowSystem(const IWindowSystem&)                    = delete;
    auto operator=(const IWindowSystem&) -> IWindowSystem& = delete;
    IWindowSystem(IWindowSystem&&)                         = delete;
    auto operator=(IWindowSystem&&) -> IWindowSystem&      = delete;

    /// @brief Concrete implementation tag for RTTI-free downcasts.
    [[nodiscard]] virtual auto GetType() const -> WindowSystemType = 0;

    [[nodiscard]] virtual auto IsValid() const -> bool = 0;

    virtual auto Shutdown() -> void = 0;

    [[nodiscard]] auto BindCommands(CommandDispatcher& Commands) -> std::expected<void, ErrorMessage> {
        return m_InputCommands.Bind(Commands);
    }

    auto UnbindCommands() -> void {
        m_InputCommands.Unbind();
    }

    /// @brief Clear held input and restore cursor presentation during owner teardown.
    auto ResetInput() -> void {
        m_InputFrameState.Shutdown();
        SetCursorMode(CursorMode::Normal);
    }

    /// @brief Pump platform events and return true when a close was requested.
    [[nodiscard]] virtual auto PollEvents() -> bool = 0;

    /// @brief Publish one frame of input executed by the Input command phase.
    auto AdvanceFrame() -> void {
        const auto& Frame = m_InputFrameState.AdvanceFrame();
        m_EventDispatcher.trigger(Frame.Keyboard);
        m_EventDispatcher.trigger(Frame.Mouse);
        const bool IsReset = std::ranges::any_of(Frame.Events, [](const InputEvent& Event) -> bool {
            return std::holds_alternative<ResetInputEvent>(Event);
        });
        if (!Frame.IsFocused || IsReset)
            SetCursorMode(CursorMode::Normal);
    }

    [[nodiscard]] auto GetInputFrame() const -> const InputFrame& {
        return m_InputFrameState.GetFrame();
    }

    /// @brief Return the dispatcher for window-system events.
    [[nodiscard]] virtual auto GetEventDispatcher() -> entt::dispatcher& = 0;

    /// @brief Return the current drawable framebuffer size in physical pixels.
    [[nodiscard]] virtual auto GetFramebufferExtent() const -> FramebufferExtent = 0;

    /// @brief Set the cursor presentation and confinement mode.
    virtual auto SetCursorMode(CursorMode Mode) -> void = 0;

  protected:
    InputFrameState      m_InputFrameState{};
    InputCommandProvider m_InputCommands;
    entt::dispatcher     m_EventDispatcher = {};
};

} // namespace SoulEngine
