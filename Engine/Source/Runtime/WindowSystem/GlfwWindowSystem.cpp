module;

#include <GLFW/glfw3.h>
#include <entt/entt.hpp>
#include <imgui.h>
#include <cstring>

module WindowSystem.Glfw;

import magic_enum;
import Core;
import WindowSystem.Types;
import WindowSystem.Interface;
import WindowSystem.ImGuiGlfwBackend;
import std;

namespace SoulEngine {

auto GLFWErrorCallback(int ErrorCode, const char* Description) -> void {
    LogError("GLFW Error [0x{:08X}]: {}", ErrorCode, Description);
}

[[nodiscard]] auto TranslateGLFWKey(int Key, int Scancode) -> int {
    if (Key >= GLFW_KEY_KP_0 && Key <= GLFW_KEY_KP_EQUAL)
        return Key;
    const auto  PreviousErrorCallback = glfwSetErrorCallback(nullptr);
    const char* KeyName               = glfwGetKeyName(Key, Scancode);
    glfwSetErrorCallback(PreviousErrorCallback);
    (void)glfwGetError(nullptr);
    if (!KeyName || KeyName[0] == 0 || KeyName[1] != 0)
        return Key;

    constexpr char         Names[] = "`-=[]\\,;'./";
    constexpr int          Keys[]  = {GLFW_KEY_GRAVE_ACCENT,
                                      GLFW_KEY_MINUS,
                                      GLFW_KEY_EQUAL,
                                      GLFW_KEY_LEFT_BRACKET,
                                      GLFW_KEY_RIGHT_BRACKET,
                                      GLFW_KEY_BACKSLASH,
                                      GLFW_KEY_COMMA,
                                      GLFW_KEY_SEMICOLON,
                                      GLFW_KEY_APOSTROPHE,
                                      GLFW_KEY_PERIOD,
                                      GLFW_KEY_SLASH};
    const std::string_view Name(KeyName);
    if (Name[0] >= '0' && Name[0] <= '9')
        return GLFW_KEY_0 + (Name[0] - '0');
    if (Name[0] >= 'A' && Name[0] <= 'Z')
        return GLFW_KEY_A + (Name[0] - 'A');
    if (Name[0] >= 'a' && Name[0] <= 'z')
        return GLFW_KEY_A + (Name[0] - 'a');
    if (const auto* Found = std::strchr(Names, Name[0]))
        return Keys[Found - Names];
    return Key;
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
    case GLFW_MOUSE_BUTTON_4:
        return WindowMouseButton::X1;
    case GLFW_MOUSE_BUTTON_5:
        return WindowMouseButton::X2;
    default:
        return WindowMouseButton::Unknown;
    }
}

[[nodiscard]] auto FromGLFWModifiers(int Mods) -> InputModifiers {
    return InputModifiers{.IsShift   = (Mods & GLFW_MOD_SHIFT) != 0,
                          .IsControl = (Mods & GLFW_MOD_CONTROL) != 0,
                          .IsAlt     = (Mods & GLFW_MOD_ALT) != 0,
                          .IsSuper   = (Mods & GLFW_MOD_SUPER) != 0};
}

GlfwWindowSystem::~GlfwWindowSystem() {
    Shutdown();
}

[[nodiscard]] auto GlfwWindowSystem::Create() -> std::expected<UPtr<GlfwWindowSystem>, ErrorMessage> {
    auto Result = std::make_unique<GlfwWindowSystem>();
    glfwSetErrorCallback(&GLFWErrorCallback);

    if (!glfwInit()) {
        const char* Desc = nullptr;
        glfwGetError(&Desc);
        return std::unexpected(ErrorMessage(Format("glfwInit failed: {}", Desc ? Desc : "unknown error")));
    }

    // GLFW was originally designed to create an OpenGL context, and we need to tell it not to do so.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    const auto WindowWidth  = CVarWindowResolutionX.Get();
    const auto WindowHeight = CVarWindowResolutionY.Get();
    // Application.Name is registered by the Launch module; the fallback here only guards against its
    // registration being dead-stripped.
    Result->m_Title         = CVarManager::Get().Find<String>("Application.Name").value_or("SoulEngine");

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
    glfwSetCharCallback(Result->m_Window, &GlfwWindowSystem::OnChar);
    glfwSetScrollCallback(Result->m_Window, &GlfwWindowSystem::OnScroll);
    glfwSetCursorPosCallback(Result->m_Window, &GlfwWindowSystem::OnCursorPosition);
    glfwSetCursorEnterCallback(Result->m_Window, &GlfwWindowSystem::OnCursorEnter);
    glfwSetMouseButtonCallback(Result->m_Window, &GlfwWindowSystem::OnMouseButton);

    // Use framebuffer size rather than window size because Vulkan renders to drawable pixels.
    // GLFW window size is logical screen coordinates and can be smaller than the framebuffer
    // on HiDPI/Retina displays.
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

auto GlfwWindowSystem::Shutdown() -> void {
    ResetInput();
    UnbindCommands();
    if (!m_bInitialized)
        return;

    if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendPlatformUserData)
        ImGuiGlfwAdapter::Shutdown();

    if (m_Window) {
        glfwDestroyWindow(m_Window);
        m_Window = nullptr;
    }

    glfwTerminate();

    m_Title        = {};
    m_Extent       = {};
    m_CursorMode   = CursorMode::Normal;
    m_bInitialized = false;
}

[[nodiscard]] auto GlfwWindowSystem::GetType() const -> WindowSystemType {
    return WindowSystemType::Glfw;
}

[[nodiscard]] auto GlfwWindowSystem::IsValid() const -> bool {
    return m_Window != nullptr;
}

[[nodiscard]] auto GlfwWindowSystem::GetNativeHandle() const -> GLFWwindow* {
    return m_Window;
}

[[nodiscard]] auto GlfwWindowSystem::GetEventDispatcher() -> entt::dispatcher& {
    return m_EventDispatcher;
}

[[nodiscard]] auto GlfwWindowSystem::GetFramebufferExtent() const -> FramebufferExtent {
    return m_Extent;
}

[[nodiscard]] auto GlfwWindowSystem::PollEvents() -> bool {
    m_InputCommands.RetryRecovery();
    glfwPollEvents();
    return m_Window && glfwWindowShouldClose(m_Window);
}

auto GlfwWindowSystem::SetCursorMode(CursorMode Mode) -> void {
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

auto GlfwWindowSystem::OnFramebufferResize(GLFWwindow* Win, int NewW, int NewH) -> void {
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

auto GlfwWindowSystem::OnWindowFocus(GLFWwindow* Win, int Focused) -> void {
    auto* Self = static_cast<GlfwWindowSystem*>(glfwGetWindowUserPointer(Win));
    if (!Self)
        return;
    (void)Self->m_InputCommands.Submit(FocusInputEvent{.IsFocused = Focused != 0});
}

auto GlfwWindowSystem::OnKey(GLFWwindow* Win, int Key, int Scancode, int Action, int Mods) -> void {
    auto* Self = static_cast<GlfwWindowSystem*>(glfwGetWindowUserPointer(Win));
    if (!Self || Key < 0 || (Action != GLFW_PRESS && Action != GLFW_RELEASE && Action != GLFW_REPEAT))
        return;

    const auto TranslatedKey    = TranslateGLFWKey(Key, Scancode);
    const auto InputActionValue = Action == GLFW_PRESS     ? InputAction::Press
                                  : Action == GLFW_RELEASE ? InputAction::Release
                                                           : InputAction::Repeat;
    (void)Self->m_InputCommands.Submit(KeyInputEvent{.Key       = FromGLFWKey(TranslatedKey),
                                                     .NativeKey = TranslatedKey,
                                                     .Scancode  = Scancode,
                                                     .Action    = InputActionValue,
                                                     .Modifiers = FromGLFWModifiers(Mods)});
}

auto GlfwWindowSystem::OnChar(GLFWwindow* Win, unsigned int Codepoint) -> void {
    auto* Self = static_cast<GlfwWindowSystem*>(glfwGetWindowUserPointer(Win));
    if (!Self)
        return;
    (void)Self->m_InputCommands.Submit(TextInputEvent{.Codepoint = Codepoint});
}

auto GlfwWindowSystem::OnScroll(GLFWwindow* Win, double XOffset, double YOffset) -> void {
    auto* Self = static_cast<GlfwWindowSystem*>(glfwGetWindowUserPointer(Win));
    if (!Self)
        return;

    (void)Self->m_InputCommands.Submit(
        WheelInputEvent{.X = static_cast<Float32>(XOffset), .Y = static_cast<Float32>(YOffset)});
}

auto GlfwWindowSystem::OnCursorPosition(GLFWwindow* Win, double XPosition, double YPosition) -> void {
    auto* Self = static_cast<GlfwWindowSystem*>(glfwGetWindowUserPointer(Win));
    if (!Self)
        return;

    (void)Self->m_InputCommands.Submit(
        PointerInputEvent{.Position = {.X = static_cast<Float32>(XPosition), .Y = static_cast<Float32>(YPosition)}});
}

auto GlfwWindowSystem::OnCursorEnter(GLFWwindow* Win, int Entered) -> void {
    auto* Self = static_cast<GlfwWindowSystem*>(glfwGetWindowUserPointer(Win));
    if (!Self)
        return;
    (void)Self->m_InputCommands.Submit(CursorEnterInputEvent{.IsInside = Entered != 0});
}

auto GlfwWindowSystem::OnMouseButton(GLFWwindow* Win, int Button, int Action, int Mods) -> void {
    auto* Self = static_cast<GlfwWindowSystem*>(glfwGetWindowUserPointer(Win));
    if (!Self)
        return;

    const auto EngineButton = FromGLFWMouseButton(Button);
    if (EngineButton == WindowMouseButton::Unknown || (Action != GLFW_PRESS && Action != GLFW_RELEASE))
        return;

    (void)Self->m_InputCommands.Submit(
        MouseButtonInputEvent{.Button    = EngineButton,
                              .Action    = Action == GLFW_PRESS ? InputAction::Press : InputAction::Release,
                              .Modifiers = FromGLFWModifiers(Mods)});
}

} // namespace SoulEngine
