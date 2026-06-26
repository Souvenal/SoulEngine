module;

#include <GLFW/glfw3.h>

export module Window;

import Core;

export import std;

using namespace SoulEngine::Core;

namespace {
auto GLFWErrorCallback(int ErrorCode, const char* Description) -> void {
    LogError("GLFW Error [0x{:08X}]: {}", ErrorCode, Description);
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
          m_Extent(std::exchange(Other.m_Extent, {})) {
        if (m_Window) {
            glfwSetWindowUserPointer(m_Window, this);
        }
    }
    auto operator=(WindowDisplay&& Other) noexcept -> WindowDisplay& {
        if (this != &Other) {
            std::swap(m_Window, Other.m_Window);
            std::swap(m_bInitialized, Other.m_bInitialized);
            std::swap(m_bFramebufferResized, Other.m_bFramebufferResized);
            std::swap(m_Title, Other.m_Title);
            std::swap(m_Extent, Other.m_Extent);
            if (m_Window) {
                glfwSetWindowUserPointer(m_Window, this);
                glfwSetFramebufferSizeCallback(m_Window, &WindowDisplay::OnFramebufferResize);
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

  private:
    static auto OnFramebufferResize(GLFWwindow* Win, int NewW, int NewH) -> void {
        auto* Self = static_cast<WindowDisplay*>(glfwGetWindowUserPointer(Win));
        if (Self) {
            Self->m_Extent.Width  = NewW;
            Self->m_Extent.Height = NewH;
            Self->m_bFramebufferResized = true;
        }
    }

    GLFWwindow*       m_Window              = nullptr;
    bool              m_bInitialized        = false;
    bool              m_bFramebufferResized = false;
    String            m_Title;
    FramebufferExtent m_Extent; ///< Current framebuffer extent (kept in sync on resize)
};

} // namespace SoulEngine
