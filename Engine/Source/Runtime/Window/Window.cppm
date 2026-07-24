module;

#include <GLFW/glfw3.h>

export module Window;

import Core;

export import std;

namespace {
auto GLFWErrorCallback(int ErrorCode, const char* Description) -> void {
    SoulEngine::LogError("GLFW Error [0x{:08X}]: {}", ErrorCode, Description);
}
} // namespace

namespace SoulEngine {

/// @brief Drawable framebuffer size in physical pixels.
///
/// Vulkan swapchains and render targets must use framebuffer pixels, not GLFW
/// window coordinates. On HiDPI/Retina displays these can differ, for example
/// a 1920x1080 window may have a larger framebuffer backing store.
export struct FramebufferExtent {
    int Width  = 0;
    int Height = 0;
};

/// @brief Relative cursor movement accumulated between game ticks.
export struct CursorDelta {
    float X = 0.0f;
    float Y = 0.0f;
};

/// @brief Keyboard keys exposed by the window input surface.
export enum class WindowKey {
    Unknown = 0,
    A,
    D,
    E,
    Q,
    S,
    W,
};

export class WindowDisplay final {
  public:
    WindowDisplay() = default;
    ~WindowDisplay() {
        Shutdown();
    }

    WindowDisplay(const WindowDisplay&)                    = delete;
    auto operator=(const WindowDisplay&) -> WindowDisplay& = delete;
    WindowDisplay(WindowDisplay&& Other) noexcept
        : m_Window(std::exchange(Other.m_Window, nullptr)),
          m_bInitialized(std::exchange(Other.m_bInitialized, false)),
          m_bFramebufferResized(std::exchange(Other.m_bFramebufferResized, false)),
          m_Title(std::move(Other.m_Title)),
          m_Extent(std::exchange(Other.m_Extent, {})),
          m_ScrollDelta(std::exchange(Other.m_ScrollDelta, 0.0f)),
          m_CursorDelta(std::exchange(Other.m_CursorDelta, {})),
          m_LastCursorX(std::exchange(Other.m_LastCursorX, 0.0)),
          m_LastCursorY(std::exchange(Other.m_LastCursorY, 0.0)),
          m_HasCursorPosition(std::exchange(Other.m_HasCursorPosition, false)) {
        if (m_Window) {
            glfwSetWindowUserPointer(m_Window, this);
            glfwSetFramebufferSizeCallback(m_Window, &WindowDisplay::OnFramebufferResize);
            glfwSetScrollCallback(m_Window, &WindowDisplay::OnScroll);
            glfwSetCursorPosCallback(m_Window, &WindowDisplay::OnCursorPosition);
        }
    }
    auto operator=(WindowDisplay&& Other) noexcept -> WindowDisplay& {
        if (this != &Other) {
            std::swap(m_Window, Other.m_Window);
            std::swap(m_bInitialized, Other.m_bInitialized);
            std::swap(m_bFramebufferResized, Other.m_bFramebufferResized);
            std::swap(m_Title, Other.m_Title);
            std::swap(m_Extent, Other.m_Extent);
            std::swap(m_ScrollDelta, Other.m_ScrollDelta);
            std::swap(m_CursorDelta, Other.m_CursorDelta);
            std::swap(m_LastCursorX, Other.m_LastCursorX);
            std::swap(m_LastCursorY, Other.m_LastCursorY);
            std::swap(m_HasCursorPosition, Other.m_HasCursorPosition);
            if (m_Window) {
                glfwSetWindowUserPointer(m_Window, this);
                glfwSetFramebufferSizeCallback(m_Window, &WindowDisplay::OnFramebufferResize);
                glfwSetScrollCallback(m_Window, &WindowDisplay::OnScroll);
                glfwSetCursorPosCallback(m_Window, &WindowDisplay::OnCursorPosition);
            }
        }
        return *this;
    }

    [[nodiscard]] static auto Create() -> std::expected<WindowDisplay, ErrorMessage> {
        WindowDisplay Result;
        glfwSetErrorCallback(&GLFWErrorCallback);

        if (!glfwInit()) {
            const char* Desc = nullptr;
            glfwGetError(&Desc);
            return std::unexpected(ErrorMessage(Format("glfwInit failed: {}", Desc ? Desc : "unknown error")));
        }

        // GLFW was originally designed to create an OpenGL context,
        // and we need to tell it not to do so.
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

        auto&      Cfg          = ConfigManager::Get().GetConfig();
        const auto WindowWidth  = Cfg.Window.ResolutionX.value_or(1280);
        const auto WindowHeight = Cfg.Window.ResolutionY.value_or(720);
        Result.m_Title          = Cfg.Application.Name.value_or("SoulEngine");

        Result.m_Window = glfwCreateWindow(WindowWidth, WindowHeight, Result.m_Title.c_str(), nullptr, nullptr);

        if (!Result.m_Window) {
            const char* Desc = nullptr;
            glfwGetError(&Desc);
            Result.Shutdown();
            return std::unexpected(ErrorMessage(Format("glfwCreateWindow failed: {}", Desc ? Desc : "unknown error")));
        }

        glfwSetWindowUserPointer(Result.m_Window, &Result);
        glfwSetFramebufferSizeCallback(Result.m_Window, &WindowDisplay::OnFramebufferResize);
        glfwSetScrollCallback(Result.m_Window, &WindowDisplay::OnScroll);
        glfwSetCursorPosCallback(Result.m_Window, &WindowDisplay::OnCursorPosition);
        glfwSetInputMode(Result.m_Window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

        // Use framebuffer size rather than window size because Vulkan renders
        // to drawable pixels. GLFW window size is logical screen coordinates
        // and can be smaller than the framebuffer on HiDPI/Retina displays.
        glfwGetFramebufferSize(Result.m_Window, &Result.m_Extent.Width, &Result.m_Extent.Height);

        Result.m_bFramebufferResized = true;
        Result.m_bInitialized        = true;

        LogInfo(
            "Window created: window={}x{}, framebuffer={}x{} \"{}\"",
            WindowWidth,
            WindowHeight,
            Result.m_Extent.Width,
            Result.m_Extent.Height,
            Result.m_Title);
        return Result;
    }

    auto Shutdown() -> void {
        if (!m_bInitialized)
            return;

        if (m_Window) {
            glfwDestroyWindow(m_Window);
            m_Window = nullptr;
        }

        glfwTerminate();

        m_Title        = {};
        m_Extent       = {};
        m_ScrollDelta  = 0.0f;
        m_CursorDelta  = {};
        m_LastCursorX  = 0.0;
        m_LastCursorY  = 0.0;
        m_HasCursorPosition = false;
        m_bInitialized = false;
    }

    [[nodiscard]] auto IsValid() const -> bool {
        return m_Window != nullptr;
    }

    [[nodiscard]] auto GetNativeHandle() const -> GLFWwindow* {
        return m_Window;
    }

    [[nodiscard]] auto ConsumeFramebufferResize() -> std::optional<FramebufferExtent> {
        if (!m_bFramebufferResized)
            return std::nullopt;

        m_bFramebufferResized = false;
        return m_Extent;
    }

    [[nodiscard]] auto PollEvents() -> bool {
        glfwPollEvents();

        return glfwWindowShouldClose(m_Window);
    }

    /// @brief Return whether a supported keyboard key is currently pressed.
    [[nodiscard]] auto IsKeyPressed(WindowKey Key) const -> bool {
        if (!m_Window)
            return false;

        const auto GLFWKey = ToGLFWKey(Key);
        return GLFWKey != GLFW_KEY_UNKNOWN && glfwGetKey(m_Window, GLFWKey) == GLFW_PRESS;
    }

    /// @brief Return and clear the vertical scroll amount received since the last call.
    auto ConsumeScrollDelta() -> float {
        return std::exchange(m_ScrollDelta, 0.0f);
    }

    /// @brief Return and clear the relative cursor movement received since the last call.
    auto ConsumeCursorDelta() -> CursorDelta {
        return std::exchange(m_CursorDelta, {});
    }

  private:
    [[nodiscard]] static auto ToGLFWKey(WindowKey Key) -> int {
        switch (Key) {
        case WindowKey::A:
            return GLFW_KEY_A;
        case WindowKey::D:
            return GLFW_KEY_D;
        case WindowKey::E:
            return GLFW_KEY_E;
        case WindowKey::Q:
            return GLFW_KEY_Q;
        case WindowKey::S:
            return GLFW_KEY_S;
        case WindowKey::W:
            return GLFW_KEY_W;
        case WindowKey::Unknown:
            break;
        }
        return GLFW_KEY_UNKNOWN;
    }

    static auto OnFramebufferResize(GLFWwindow* Win, int NewW, int NewH) -> void {
        auto* Self = static_cast<WindowDisplay*>(glfwGetWindowUserPointer(Win));
        if (Self) {
            Self->m_Extent.Width  = NewW;
            Self->m_Extent.Height = NewH;
            Self->m_bFramebufferResized = true;
        }
    }

    static auto OnScroll(GLFWwindow* Win, double /*XOffset*/, double YOffset) -> void {
        auto* Self = static_cast<WindowDisplay*>(glfwGetWindowUserPointer(Win));
        if (Self)
            Self->m_ScrollDelta += static_cast<float>(YOffset);
    }

    static auto OnCursorPosition(GLFWwindow* Win, double XPosition, double YPosition) -> void {
        auto* Self = static_cast<WindowDisplay*>(glfwGetWindowUserPointer(Win));
        if (!Self)
            return;
        if (!Self->m_HasCursorPosition) {
            Self->m_LastCursorX       = XPosition;
            Self->m_LastCursorY       = YPosition;
            Self->m_HasCursorPosition = true;
            return;
        }

        Self->m_CursorDelta.X += static_cast<float>(XPosition - Self->m_LastCursorX);
        Self->m_CursorDelta.Y += static_cast<float>(YPosition - Self->m_LastCursorY);
        Self->m_LastCursorX = XPosition;
        Self->m_LastCursorY = YPosition;
    }

    GLFWwindow*       m_Window              = nullptr;
    bool              m_bInitialized        = false;
    bool              m_bFramebufferResized = false;
    String            m_Title;
    FramebufferExtent m_Extent; ///< Current framebuffer extent (kept in sync on resize)
    float             m_ScrollDelta         = 0.0f;
    CursorDelta       m_CursorDelta         = {};
    double            m_LastCursorX         = 0.0;
    double            m_LastCursorY         = 0.0;
    bool              m_HasCursorPosition   = false;
};

} // namespace SoulEngine
