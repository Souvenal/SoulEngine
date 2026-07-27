export module WindowSystem:Types;

export import std;

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

/// @brief Relative cursor movement accumulated between game ticks.
struct CursorDelta {
    float X = 0.0f;
    float Y = 0.0f;
};

/// @brief Keyboard keys exposed by the window input surface.
enum class WindowKey {
    Unknown = 0,
    A,
    D,
    E,
    Q,
    S,
    W,
};

/// @brief Mouse buttons exposed by the window input surface.
enum class WindowMouseButton {
    Unknown = 0,
    Left,
    Middle,
    Right,
};

} // namespace SoulEngine
