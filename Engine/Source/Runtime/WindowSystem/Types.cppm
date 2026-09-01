module;

export module WindowSystem:Types;

import magic_enum;
export import std;
import Core;

export namespace SoulEngine {

/// @brief Identifies the concrete window-system implementation behind
/// IWindowSystem.
///
/// RTTI is disabled engine-wide, so consumers dispatch on this tag before
/// static_cast-ing an IWindowSystem reference to the concrete implementation.
enum class WindowSystemType {
    Unknown = 0,
    Glfw,
};

/// @brief Drawable framebuffer size in physical pixels.
///
/// Vulkan swapchains and render targets must use framebuffer pixels, not GLFW
/// window coordinates. On HiDPI/Retina displays these can differ, for example
/// a 1920x1080 window may have a larger framebuffer backing store.
struct FramebufferExtent {
    int Width  = 0;
    int Height = 0;
};

/// @brief Notification that the drawable framebuffer size changed.
///
/// Published synchronously from the framebuffer callback. The initial window
/// extent is triggered by the consumer after it subscribes.
struct FramebufferResizeEvent {
    FramebufferExtent PreviousExtent = {};
    FramebufferExtent CurrentExtent  = {};
};

/// @brief Cursor position in window coordinates.
struct CursorPosition {
    Float32 X = 0.0f;
    Float32 Y = 0.0f;
};

/// @brief Window cursor presentation and confinement mode.
enum class CursorMode {
    Unknown = 0,
    Normal,
    Hidden,
    Disabled,
    Captured,
};

/// @brief Per-frame state of a keyboard key or mouse button.
enum class InputState {
    Unknown = 0,
    Up,
    Pressed,
    Held,
    Released,
};

/// @brief Keyboard keys exposed by the window input surface.
enum class WindowKey {
    Unknown = 0,
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    Num0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    Escape,
    Enter,
    Tab,
    Backspace,
    Space,
    Left,
    Right,
    Up,
    Down,
    LeftShift,
    RightShift,
    LeftControl,
    RightControl,
    LeftAlt,
    RightAlt,
    LeftSuper,
    RightSuper,
};

/// @brief Mouse buttons exposed by the window input surface.
enum class WindowMouseButton {
    Unknown = 0,
    Left,
    Middle,
    Right,
};

/// @brief Per-frame keyboard input snapshot.
struct KeyboardFrameEvent {
    std::array<InputState, magic_enum::enum_count<WindowKey>()> States = {};

    /// @brief Return whether a key is currently pressed or held.
    [[nodiscard]] auto IsKeyDown(WindowKey Key) const -> bool {
        const auto Index = std::to_underlying(Key);
        if (Index >= States.size())
            return false;
        return States[Index] == InputState::Pressed || States[Index] == InputState::Held;
    }
};

/// @brief Per-frame mouse input snapshot.
///
/// Cursor movement and scroll values are accumulated between frame polls.
struct MouseFrameEvent {
    std::array<InputState, magic_enum::enum_count<WindowMouseButton>()> ButtonStates = {};
    CursorPosition PreviousCursorPosition = {};
    CursorPosition CurrentCursorPosition  = {};
    Float32 ScrollX      = 0.0f;
    Float32 ScrollY      = 0.0f;

    /// @brief Return cursor movement since the previous published frame.
    [[nodiscard]] auto GetCursorDelta() const -> CursorPosition {
        return CursorPosition{
            .X = CurrentCursorPosition.X - PreviousCursorPosition.X,
            .Y = CurrentCursorPosition.Y - PreviousCursorPosition.Y,
        };
    }

    /// @brief Return whether a mouse button is currently pressed or held.
    [[nodiscard]] auto IsButtonDown(WindowMouseButton Button) const -> bool {
        const auto Index = std::to_underlying(Button);
        if (Index >= ButtonStates.size())
            return false;
        return ButtonStates[Index] == InputState::Pressed || ButtonStates[Index] == InputState::Held;
    }
};

} // namespace SoulEngine
