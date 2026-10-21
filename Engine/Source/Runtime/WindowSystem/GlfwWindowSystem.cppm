module;

#include <GLFW/glfw3.h>
#include <entt/entt.hpp>
#include <imgui_impl_glfw.h>

export module WindowSystem.Glfw;

import magic_enum;
import Core;
import WindowSystem.Types;
import WindowSystem.Interface;

import std;

namespace SoulEngine {

export class GlfwWindowSystem final : public IWindowSystem {
  public:
    GlfwWindowSystem() = default;
    ~GlfwWindowSystem() override;

    GlfwWindowSystem(const GlfwWindowSystem&)                    = delete;
    auto operator=(const GlfwWindowSystem&) -> GlfwWindowSystem& = delete;
    GlfwWindowSystem(GlfwWindowSystem&&)                         = delete;
    auto operator=(GlfwWindowSystem&&) -> GlfwWindowSystem&      = delete;

    [[nodiscard]] static auto Create() -> std::expected<UPtr<GlfwWindowSystem>, ErrorMessage>;

    auto Shutdown() -> void override;

    [[nodiscard]] auto GetType() const -> WindowSystemType override;

    [[nodiscard]] auto IsValid() const -> bool override;

    [[nodiscard]] auto GetNativeHandle() const -> GLFWwindow*;

    [[nodiscard]] auto GetEventDispatcher() -> entt::dispatcher& override;

    [[nodiscard]] auto GetFramebufferExtent() const -> FramebufferExtent override;

    [[nodiscard]] auto Tick() -> bool override;

    auto SetCursorMode(CursorMode Mode) -> void override;

  private:
    enum class InputTransition {
        Press,
        Release,
    };

    static auto OnFramebufferResize(GLFWwindow* Win, int NewW, int NewH) -> void;

    static auto OnWindowFocus(GLFWwindow* Win, int Focused) -> void;

    static auto OnKey(GLFWwindow* Win, int Key, int /*Scancode*/, int Action, int /*Mods*/) -> void;

    static auto OnScroll(GLFWwindow* Win, double XOffset, double YOffset) -> void;

    static auto OnCursorPosition(GLFWwindow* Win, double XPosition, double YPosition) -> void;

    static auto OnMouseButton(GLFWwindow* Win, int Button, int Action, int /*Mods*/) -> void;

    GLFWwindow*       m_Window       = nullptr;
    bool              m_bInitialized = false;
    String            m_Title;
    FramebufferExtent m_Extent; ///< Current framebuffer extent (kept in sync on resize)
    std::array<std::queue<InputTransition>, magic_enum::enum_count<WindowKey>()>         m_KeySequences         = {};
    std::array<std::queue<InputTransition>, magic_enum::enum_count<WindowMouseButton>()> m_MouseButtonSequences = {};
    CursorMode m_CursorMode = CursorMode::Normal;
};

} // namespace SoulEngine
