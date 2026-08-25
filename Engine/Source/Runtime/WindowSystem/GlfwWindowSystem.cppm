module;

#include <GLFW/glfw3.h>
#include <entt/entt.hpp>
#include <imgui_impl_glfw.h>

export module WindowSystem:Glfw;

import magic_enum;
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

[[nodiscard]] auto FromGLFWKey(int Key) -> WindowKey {
    if (Key >= GLFW_KEY_A && Key <= GLFW_KEY_Z) {
        auto Offset = Key - GLFW_KEY_A;
        return static_cast<WindowKey>(std::to_underlying<WindowKey>(WindowKey::A) + Offset);
    } else if (Key >= GLFW_KEY_0 && Key <= GLFW_KEY_9) {
        auto Offset = Key - GLFW_KEY_0;
        return static_cast<WindowKey>(std::to_underlying<WindowKey>(WindowKey::Num0) + Offset);
    } else if (Key >= GLFW_KEY_F1 && Key <= GLFW_KEY_F12) {
        auto Offset = Key - GLFW_KEY_F1;
        return static_cast<WindowKey>(std::to_underlying<WindowKey>(WindowKey::F1) + Offset);
    }

    switch (Key) {
    case GLFW_KEY_ESCAPE:
        return WindowKey::Escape;
    case GLFW_KEY_ENTER:
        return WindowKey::Enter;
    case GLFW_KEY_TAB:
        return WindowKey::Tab;
    case GLFW_KEY_BACKSPACE:
        return WindowKey::Backspace;
    case GLFW_KEY_SPACE:
        return WindowKey::Space;
    case GLFW_KEY_LEFT:
        return WindowKey::Left;
    case GLFW_KEY_RIGHT:
        return WindowKey::Right;
    case GLFW_KEY_UP:
        return WindowKey::Up;
    case GLFW_KEY_DOWN:
        return WindowKey::Down;
    case GLFW_KEY_LEFT_SHIFT:
        return WindowKey::LeftShift;
    case GLFW_KEY_RIGHT_SHIFT:
        return WindowKey::RightShift;
    case GLFW_KEY_LEFT_CONTROL:
        return WindowKey::LeftControl;
    case GLFW_KEY_RIGHT_CONTROL:
        return WindowKey::RightControl;
    case GLFW_KEY_LEFT_ALT:
        return WindowKey::LeftAlt;
    case GLFW_KEY_RIGHT_ALT:
        return WindowKey::RightAlt;
    case GLFW_KEY_LEFT_SUPER:
        return WindowKey::LeftSuper;
    case GLFW_KEY_RIGHT_SUPER:
        return WindowKey::RightSuper;
    default:
        return WindowKey::Unknown;
    }
}

[[nodiscard]] auto FromGLFWMouseButton(int Button) -> WindowMouseButton {
    switch (Button) {
    case GLFW_MOUSE_BUTTON_LEFT:
        return WindowMouseButton::Left;
    case GLFW_MOUSE_BUTTON_MIDDLE:
        return WindowMouseButton::Middle;
    case GLFW_MOUSE_BUTTON_RIGHT:
        return WindowMouseButton::Right;
    default:
        return WindowMouseButton::Unknown;
    }
}

} // namespace

/// @brief GLFW implementation of IWindowSystem.
///
/// Owns the GLFW library lifetime and one native window: surface creation,
/// event polling, input state tracking, scroll accumulation, and relative
/// cursor accumulation. Non-copyable and non-movable.
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
    GlfwWindowSystem(GlfwWindowSystem&&)                         = delete;
    auto operator=(GlfwWindowSystem&&) -> GlfwWindowSystem&      = delete;

    [[nodiscard]] static auto Create() -> std::expected<UPtr<GlfwWindowSystem>, ErrorMessage> {
        auto Result = std::make_unique<GlfwWindowSystem>();
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
        Result->m_Title         = Cfg.Application.Name.value_or("SoulEngine");

        Result->m_Window = glfwCreateWindow(WindowWidth, WindowHeight, Result->m_Title.c_str(), nullptr, nullptr);

        if (!Result->m_Window) {
            const char* Desc = nullptr;
            glfwGetError(&Desc);
            Result->Shutdown();
            return std::unexpected(ErrorMessage(Format("glfwCreateWindow failed: {}", Desc ? Desc : "unknown error")));
        }

        glfwSetWindowUserPointer(Result->m_Window, Result.get());
        glfwSetFramebufferSizeCallback(Result->m_Window, &GlfwWindowSystem::OnFramebufferResize);
        glfwSetWindowFocusCallback(Result->m_Window, &GlfwWindowSystem::OnWindowFocus);
        glfwSetKeyCallback(Result->m_Window, &GlfwWindowSystem::OnKey);
        glfwSetScrollCallback(Result->m_Window, &GlfwWindowSystem::OnScroll);
        glfwSetCursorPosCallback(Result->m_Window, &GlfwWindowSystem::OnCursorPosition);
        glfwSetMouseButtonCallback(Result->m_Window, &GlfwWindowSystem::OnMouseButton);

        // Use framebuffer size rather than window size because Vulkan renders
        // to drawable pixels. GLFW window size is logical screen coordinates
        // and can be smaller than the framebuffer on HiDPI/Retina displays.
        glfwGetFramebufferSize(Result->m_Window, &Result->m_Extent.Width, &Result->m_Extent.Height);

        Result->m_bInitialized = true;

        LogInfo("Window created: window={}x{}, framebuffer={}x{} \"{}\"",
                WindowWidth,
                WindowHeight,
                Result->m_Extent.Width,
                Result->m_Extent.Height,
                Result->m_Title);
        return Result;
    }

    auto Shutdown() -> void override {
        if (!m_bInitialized)
            return;

        if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendPlatformUserData)
            ImGui_ImplGlfw_Shutdown();

        if (m_Window) {
            glfwDestroyWindow(m_Window);
            m_Window = nullptr;
        }

        glfwTerminate();

        m_Title                = {};
        m_Extent               = {};
        m_KeySequences         = {};
        m_MouseButtonSequences = {};
        m_MouseFrameEvent      = {};
        m_CursorMode           = CursorMode::Normal;
        m_bInitialized         = false;
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

    [[nodiscard]] auto GetEventDispatcher() -> entt::dispatcher& override {
        return m_EventDispatcher;
    }

    [[nodiscard]] auto GetFramebufferExtent() const -> FramebufferExtent override {
        return m_Extent;
    }

    [[nodiscard]] auto Tick() -> bool override {
        glfwPollEvents();

        // GLFW can invoke both press and release callbacks before one engine
        // tick. Consuming at most one transition per tick preserves both
        // frame-visible states instead of allowing the later release to
        // overwrite the press. When a sequence is empty, transient states
        // settle into their stable states: Pressed becomes Held and Released
        // becomes Up.
        const auto UpdateInputState = [](InputState& State, std::queue<InputTransition>& Sequence) -> void {
            // A queued transition represents an input callback that occurred
            // during the previous platform-event pump. It has priority over
            // the normal state progression, and exactly one transition is
            // consumed per tick so Press and Release remain observable on
            // separate frames.
            if (Sequence.empty()) {
                // No callback occurred for this input. Pressed and Released
                // are one-frame edge states, so they settle only after a tick
                // in which there is no newer transition to consume.
                if (State == InputState::Pressed)
                    State = InputState::Held;
                else if (State == InputState::Released)
                    State = InputState::Up;
                else if (State == InputState::Unknown)
                    State = InputState::Up;
                return;
            }

            const auto Transition = Sequence.front();
            Sequence.pop();
            if (Transition == InputTransition::Press) {
                // A press always starts a new Pressed frame, even if the
                // previous state was Held, Released, or Up.
                State = InputState::Pressed;
                return;
            }

            // Release is meaningful only while the input is currently down.
            // This prevents a focus-loss release appended to an already-up
            // input from producing a second, artificial Released frame.
            if (State == InputState::Pressed || State == InputState::Held) {
                State = InputState::Released;
                return;
            }

            // The release was stale, but it was still consumed. Do not run
            // the no-transition decay above in this tick.
        };

        for (std::size_t Index = 1; Index < magic_enum::enum_count<WindowKey>(); ++Index)
            UpdateInputState(m_KeyboardFrameEvent.States[Index], m_KeySequences[Index]);

        for (std::size_t Index = 1; Index < magic_enum::enum_count<WindowMouseButton>(); ++Index)
            UpdateInputState(m_MouseFrameEvent.ButtonStates[Index], m_MouseButtonSequences[Index]);

        m_EventDispatcher.trigger(m_KeyboardFrameEvent);
        m_EventDispatcher.trigger(m_MouseFrameEvent);

        m_MouseFrameEvent.PreviousCursorPosition = m_MouseFrameEvent.CurrentCursorPosition;
        m_MouseFrameEvent.ScrollX                = 0.0f;
        m_MouseFrameEvent.ScrollY                = 0.0f;

        return m_Window && glfwWindowShouldClose(m_Window);
    }

    auto SetCursorMode(CursorMode Mode) -> void override {
        if (!m_Window || Mode == CursorMode::Unknown || m_CursorMode == Mode)
            return;

        int GLFWMode = GLFW_CURSOR_NORMAL;
        switch (Mode) {
        case CursorMode::Normal:
            GLFWMode = GLFW_CURSOR_NORMAL;
            break;
        case CursorMode::Hidden:
            GLFWMode = GLFW_CURSOR_HIDDEN;
            break;
        case CursorMode::Disabled:
            GLFWMode = GLFW_CURSOR_DISABLED;
            break;
        case CursorMode::Captured:
            GLFWMode = GLFW_CURSOR_CAPTURED;
            break;
        case CursorMode::Unknown:
            return;
        }

        glfwSetInputMode(m_Window, GLFW_CURSOR, GLFWMode);
        m_CursorMode = Mode;
    }

  private:
    enum class InputTransition {
        Press,
        Release,
    };

    static auto OnFramebufferResize(GLFWwindow* Win, int NewW, int NewH) -> void {
        auto* Self = static_cast<GlfwWindowSystem*>(glfwGetWindowUserPointer(Win));
        if (!Self)
            return;

        const auto PreviousExtent = Self->m_Extent;
        Self->m_Extent            = FramebufferExtent{.Width = NewW, .Height = NewH};
        Self->m_EventDispatcher.trigger(FramebufferResizeEvent{
            .PreviousExtent = PreviousExtent,
            .CurrentExtent  = Self->m_Extent,
        });
    }

    static auto OnWindowFocus(GLFWwindow* Win, int Focused) -> void {
        auto* Self = static_cast<GlfwWindowSystem*>(glfwGetWindowUserPointer(Win));
        if (!Self || Focused)
            return;

        // Focus loss invalidates every platform-held input. Append releases
        // after existing transitions so the original callback order remains
        // observable before the forced release is consumed.
        for (std::size_t Index = 1; Index < Self->m_KeySequences.size(); ++Index)
            Self->m_KeySequences[Index].emplace(InputTransition::Release);
        for (std::size_t Index = 1; Index < Self->m_MouseButtonSequences.size(); ++Index)
            Self->m_MouseButtonSequences[Index].emplace(InputTransition::Release);
    }

    static auto OnKey(GLFWwindow* Win, int Key, int /*Scancode*/, int Action, int /*Mods*/) -> void {
        auto* Self = static_cast<GlfwWindowSystem*>(glfwGetWindowUserPointer(Win));
        if (!Self)
            return;

        const auto EngineKey = FromGLFWKey(Key);
        if (EngineKey == WindowKey::Unknown)
            return;

        if (Action == GLFW_PRESS)
            Self->m_KeySequences[std::to_underlying(EngineKey)].emplace(InputTransition::Press);
        else if (Action == GLFW_RELEASE)
            Self->m_KeySequences[std::to_underlying(EngineKey)].emplace(InputTransition::Release);
    }

    static auto OnScroll(GLFWwindow* Win, double XOffset, double YOffset) -> void {
        auto* Self = static_cast<GlfwWindowSystem*>(glfwGetWindowUserPointer(Win));
        if (!Self)
            return;

        Self->m_MouseFrameEvent.ScrollX += static_cast<Float32>(XOffset);
        Self->m_MouseFrameEvent.ScrollY += static_cast<Float32>(YOffset);
    }

    static auto OnCursorPosition(GLFWwindow* Win, double XPosition, double YPosition) -> void {
        auto* Self = static_cast<GlfwWindowSystem*>(glfwGetWindowUserPointer(Win));
        if (!Self)
            return;

        const CursorPosition Current{
            .X = static_cast<Float32>(XPosition),
            .Y = static_cast<Float32>(YPosition),
        };
        Self->m_MouseFrameEvent.CurrentCursorPosition = Current;
    }

    static auto OnMouseButton(GLFWwindow* Win, int Button, int Action, int /*Mods*/) -> void {
        auto* Self = static_cast<GlfwWindowSystem*>(glfwGetWindowUserPointer(Win));
        if (!Self)
            return;

        const auto EngineButton = FromGLFWMouseButton(Button);
        if (EngineButton == WindowMouseButton::Unknown)
            return;

        if (Action == GLFW_PRESS)
            Self->m_MouseButtonSequences[std::to_underlying(EngineButton)].emplace(InputTransition::Press);
        else if (Action == GLFW_RELEASE)
            Self->m_MouseButtonSequences[std::to_underlying(EngineButton)].emplace(InputTransition::Release);
    }

    GLFWwindow*       m_Window       = nullptr;
    bool              m_bInitialized = false;
    String            m_Title;
    FramebufferExtent m_Extent; ///< Current framebuffer extent (kept in sync on resize)
    std::array<std::queue<InputTransition>, magic_enum::enum_count<WindowKey>()>         m_KeySequences         = {};
    std::array<std::queue<InputTransition>, magic_enum::enum_count<WindowMouseButton>()> m_MouseButtonSequences = {};
    CursorMode m_CursorMode = CursorMode::Normal;
};

} // namespace SoulEngine
