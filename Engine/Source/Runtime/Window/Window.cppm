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

struct WindowDesc {
    String Title;
    int    Width  = 0;
    int    Height = 0;
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
          m_bExitRequested(std::exchange(Other.m_bExitRequested, false)),
          m_bInitialized(std::exchange(Other.m_bInitialized, false)),
          m_Desc(std::move(Other.m_Desc)) {
        if (m_Window) {
            glfwSetWindowUserPointer(m_Window, this);
        }
    }
    auto operator=(WindowDisplay&& Other) noexcept -> WindowDisplay& {
        if (this != &Other) {
            std::swap(m_Window, Other.m_Window);
            std::swap(m_bExitRequested, Other.m_bExitRequested);
            std::swap(m_bInitialized, Other.m_bInitialized);
            std::swap(m_Desc, Other.m_Desc);
            if (m_Window) {
                glfwSetWindowUserPointer(m_Window, this);
                glfwSetWindowSizeCallback(m_Window, &WindowDisplay::OnResize);
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

        auto& Cfg     = ConfigManager::Get().GetConfig();
        Result.m_Desc = WindowDesc{
            .Title  = Cfg.Application.Name.value_or("SoulEngine"),
            .Width  = Cfg.Window.ResolutionX.value_or(1280),
            .Height = Cfg.Window.ResolutionY.value_or(720),
        };

        Result.m_Window =
            glfwCreateWindow(Result.m_Desc.Width, Result.m_Desc.Height, Result.m_Desc.Title.c_str(), nullptr, nullptr);

        if (!Result.m_Window) {
            const char* Desc = nullptr;
            glfwGetError(&Desc);
            Result.Shutdown();
            return std::unexpected(ErrorMessage(Format("glfwCreateWindow failed: {}", Desc ? Desc : "unknown error")));
        }

        glfwSetWindowUserPointer(Result.m_Window, &Result);
        glfwSetWindowSizeCallback(Result.m_Window, &WindowDisplay::OnResize);

        Result.m_bInitialized = true;

        LogInfo("Window created: {}x{} \"{}\"", Result.m_Desc.Width, Result.m_Desc.Height, Result.m_Desc.Title);
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

        m_Desc         = {};
        m_bInitialized = false;
    }

    [[nodiscard]] auto IsValid() const -> bool {
        return m_Window != nullptr;
    }

    [[nodiscard]] auto GetNativeHandle() const -> GLFWwindow* {
        return m_Window;
    }

    [[nodiscard]] auto GetWidth() const -> int {
        return m_Desc.Width;
    }
    [[nodiscard]] auto GetHeight() const -> int {
        return m_Desc.Height;
    }

    auto PollEvents() -> void {
        glfwPollEvents();

        if (glfwWindowShouldClose(m_Window)) {
            m_bExitRequested = true;
        }
    }

    [[nodiscard]] auto IsExitRequested() const -> bool {
        return m_bExitRequested;
    }

  private:
    static auto OnResize(GLFWwindow* Win, int NewW, int NewH) -> void {
        auto* Self = static_cast<WindowDisplay*>(glfwGetWindowUserPointer(Win));
        if (Self) {
            Self->m_Desc.Width  = NewW;
            Self->m_Desc.Height = NewH;
        }
    }

    GLFWwindow* m_Window         = nullptr;
    bool        m_bExitRequested = false;
    bool        m_bInitialized   = false;
    WindowDesc  m_Desc; ///< Current window state (kept in sync on resize)
};

} // namespace SoulEngine
