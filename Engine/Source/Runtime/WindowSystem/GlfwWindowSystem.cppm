module;

#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>

export module WindowSystem:Glfw;

import Core;
import :Types;
import :Interface;

export import std;

namespace SoulEngine {

namespace {
    
constexpr Int32 DefaultWindowWidth  = 1280;
constexpr Int32 DefaultWindowHeight = 720;

auto GLFWErrorCallback(int ErrorCode, const char* Description) -> void {
    LogError("GLFW Error [0x{:08X}]: {}", ErrorCode, Description);
}

[[nodiscard]] auto ToGLFWKey(WindowKey Key) -> int {
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

[[nodiscard]] auto ToGLFWMouseButton(WindowMouseButton Button) -> int {
    switch (Button) {
    case WindowMouseButton::Left:
        return GLFW_MOUSE_BUTTON_LEFT;
    case WindowMouseButton::Middle:
        return GLFW_MOUSE_BUTTON_MIDDLE;
    case WindowMouseButton::Right:
        return GLFW_MOUSE_BUTTON_RIGHT;
    case WindowMouseButton::Unknown:
        break;
    }
    return -1;
}

} // namespace

/// @brief GLFW implementation of IWindowSystem.
///
/// Owns the GLFW library lifetime and one native window: surface creation,
/// event polling, supported-key state queries, scroll accumulation, and
/// relative cursor accumulation. Non-copyable and movable.
///
/// Backend-native consumers (Vulkan surface creation, the ImGui GLFW
/// backend) obtain the raw GLFWwindow* via GetNativeHandle() after
/// dispatching on IWindowSystem::GetType().
export class GlfwWindowSystem final : public IWindowSystem {
  public:
    GlfwWindowSystem() = default;
    ~GlfwWindowSystem() override {
        Shutdown();
    }

    GlfwWindowSystem(const GlfwWindowSystem&)                    = delete;
    auto operator=(const GlfwWindowSystem&) -> GlfwWindowSystem& = delete;
    GlfwWindowSystem(GlfwWindowSystem&& Other) noexcept
        : m_Window(std::exchange(Other.m_Window, nullptr)),
          m_bInitialized(std::exchange(Other.m_bInitialized, false)),
          m_bFramebufferResized(std::exchange(Other.m_bFramebufferResized, false)),
          m_Title(std::move(Other.m_Title)),
          m_Extent(std::exchange(Other.m_Extent, {})),
          m_ScrollDelta(std::exchange(Other.m_ScrollDelta, 0.0f)),
          m_CursorDelta(std::exchange(Other.m_CursorDelta, {})),
          m_LastCursorX(std::exchange(Other.m_LastCursorX, 0.0)),
          m_LastCursorY(std::exchange(Other.m_LastCursorY, 0.0)),
          m_HasCursorPosition(std::exchange(Other.m_HasCursorPosition, false)),
          m_CursorCaptured(std::exchange(Other.m_CursorCaptured, false)) {
        if (m_Window) {
            glfwSetWindowUserPointer(m_Window, this);
            glfwSetFramebufferSizeCallback(m_Window, &GlfwWindowSystem::OnFramebufferResize);
            glfwSetScrollCallback(m_Window, &GlfwWindowSystem::OnScroll);
            glfwSetCursorPosCallback(m_Window, &GlfwWindowSystem::OnCursorPosition);
        }
    }
    auto operator=(GlfwWindowSystem&& Other) noexcept -> GlfwWindowSystem& {
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
            std::swap(m_CursorCaptured, Other.m_CursorCaptured);
            if (m_Window) {
                glfwSetWindowUserPointer(m_Window, this);
                glfwSetFramebufferSizeCallback(m_Window, &GlfwWindowSystem::OnFramebufferResize);
                glfwSetScrollCallback(m_Window, &GlfwWindowSystem::OnScroll);
                glfwSetCursorPosCallback(m_Window, &GlfwWindowSystem::OnCursorPosition);
            }
        }
        return *this;
    }

    [[nodiscard]] static auto Create() -> std::expected<GlfwWindowSystem, ErrorMessage> {
        GlfwWindowSystem Result;
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
        const auto WindowWidth  = Cfg.Window.ResolutionX.value_or(DefaultWindowWidth);
        const auto WindowHeight = Cfg.Window.ResolutionY.value_or(DefaultWindowHeight);
        Result.m_Title          = Cfg.Application.Name.value_or("SoulEngine");

        Result.m_Window = glfwCreateWindow(WindowWidth, WindowHeight, Result.m_Title.c_str(), nullptr, nullptr);

        if (!Result.m_Window) {
            const char* Desc = nullptr;
            glfwGetError(&Desc);
            Result.Shutdown();
            return std::unexpected(ErrorMessage(Format("glfwCreateWindow failed: {}", Desc ? Desc : "unknown error")));
        }

        glfwSetWindowUserPointer(Result.m_Window, &Result);
        glfwSetFramebufferSizeCallback(Result.m_Window, &GlfwWindowSystem::OnFramebufferResize);
        glfwSetScrollCallback(Result.m_Window, &GlfwWindowSystem::OnScroll);
        glfwSetCursorPosCallback(Result.m_Window, &GlfwWindowSystem::OnCursorPosition);

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

    auto Shutdown() -> void override {
        if (!m_bInitialized)
            return;

        if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendPlatformUserData) {
            ImGui_ImplGlfw_Shutdown();
        }

        if (m_Window) {
            glfwDestroyWindow(m_Window);
            m_Window = nullptr;
        }

        glfwTerminate();

        m_Title             = {};
        m_Extent            = {};
        m_ScrollDelta       = 0.0f;
        m_CursorDelta       = {};
        m_LastCursorX       = 0.0;
        m_LastCursorY       = 0.0;
        m_HasCursorPosition = false;
        m_CursorCaptured    = false;
        m_bInitialized      = false;
    }

    [[nodiscard]] auto GetType() const -> WindowSystemType override {
        return WindowSystemType::Glfw;
    }

    [[nodiscard]] auto IsValid() const -> bool override {
        return m_Window != nullptr;
    }

    [[nodiscard]] auto GetNativeHandle() const -> GLFWwindow* {
        return m_Window;
    }

    [[nodiscard]] auto ConsumeFramebufferResize() -> std::optional<FramebufferExtent> override {
        if (!m_bFramebufferResized)
            return std::nullopt;

        m_bFramebufferResized = false;
        return m_Extent;
    }

    [[nodiscard]] auto GetFramebufferExtent() const -> FramebufferExtent override {
        return m_Extent;
    }

    [[nodiscard]] auto PollEvents() -> bool override {
        glfwPollEvents();

        return glfwWindowShouldClose(m_Window);
    }

    /// @brief Return whether a supported keyboard key is currently pressed.
    [[nodiscard]] auto IsKeyPressed(WindowKey Key) const -> bool override {
        if (!m_Window)
            return false;

        const auto GLFWKey = ToGLFWKey(Key);
        return GLFWKey != GLFW_KEY_UNKNOWN && glfwGetKey(m_Window, GLFWKey) == GLFW_PRESS;
    }

    [[nodiscard]] auto IsMouseButtonPressed(WindowMouseButton Button) const -> bool override {
        if (!m_Window)
            return false;

        const auto GLFWButton = ToGLFWMouseButton(Button);
        return GLFWButton >= 0 && glfwGetMouseButton(m_Window, GLFWButton) == GLFW_PRESS;
    }

    auto SetCursorCaptured(bool Captured) -> void override {
        if (!m_Window || m_CursorCaptured == Captured)
            return;

        glfwSetInputMode(m_Window, GLFW_CURSOR, Captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        m_CursorCaptured    = Captured;
        m_CursorDelta       = {};
        m_HasCursorPosition = false;
    }

    /// @brief Return and clear the vertical scroll amount received since the last call.
    auto ConsumeScrollDelta() -> float override {
        return std::exchange(m_ScrollDelta, 0.0f);
    }

    /// @brief Return and clear the relative cursor movement received since the last call.
    auto ConsumeCursorDelta() -> CursorDelta override {
        return std::exchange(m_CursorDelta, {});
    }

  private:
    static auto OnFramebufferResize(GLFWwindow* Win, int NewW, int NewH) -> void {
        auto* Self = static_cast<GlfwWindowSystem*>(glfwGetWindowUserPointer(Win));
        if (Self) {
            Self->m_Extent.Width        = NewW;
            Self->m_Extent.Height       = NewH;
            Self->m_bFramebufferResized = true;
        }
    }

    static auto OnScroll(GLFWwindow* Win, double /*XOffset*/, double YOffset) -> void {
        auto* Self = static_cast<GlfwWindowSystem*>(glfwGetWindowUserPointer(Win));
        if (Self)
            Self->m_ScrollDelta += static_cast<float>(YOffset);
    }

    static auto OnCursorPosition(GLFWwindow* Win, double XPosition, double YPosition) -> void {
        auto* Self = static_cast<GlfwWindowSystem*>(glfwGetWindowUserPointer(Win));
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
    float             m_ScrollDelta       = 0.0f;
    CursorDelta       m_CursorDelta       = {};
    double            m_LastCursorX       = 0.0;
    double            m_LastCursorY       = 0.0;
    bool              m_HasCursorPosition = false;
    bool              m_CursorCaptured    = false;
};

} // namespace SoulEngine
