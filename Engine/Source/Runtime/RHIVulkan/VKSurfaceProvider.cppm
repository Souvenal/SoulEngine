module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

export module RHIVulkan.SurfaceProvider;

import magic_enum;
import WindowSystem;
import RHI;
import vulkan;
import std;
import Core;

namespace SoulEngine {

/// @brief Creates a Vulkan presentation surface for one window-system implementation.
export class IVulkanSurfaceProvider {
  public:
    IVulkanSurfaceProvider()          = default;
    virtual ~IVulkanSurfaceProvider() = default;

    IVulkanSurfaceProvider(const IVulkanSurfaceProvider&)                    = delete;
    auto operator=(const IVulkanSurfaceProvider&) -> IVulkanSurfaceProvider& = delete;
    IVulkanSurfaceProvider(IVulkanSurfaceProvider&&)                         = delete;
    auto operator=(IVulkanSurfaceProvider&&) -> IVulkanSurfaceProvider&      = delete;

    /// @brief Return the Vulkan instance extensions required by the window system.
    [[nodiscard]] virtual auto GetRequiredInstanceExtensions() const
        -> std::expected<std::vector<const char*>, ErrorMessage> = 0;

    /// @brief Create the presentation surface owned by the Vulkan render device.
    [[nodiscard]] virtual auto CreateSurface(vk::raii::Instance& Instance)
        -> std::expected<vk::raii::SurfaceKHR, ErrorMessage> = 0;

    /// @brief Return the current drawable framebuffer extent in physical pixels.
    [[nodiscard]] virtual auto GetFramebufferExtent() const -> FramebufferExtent = 0;
};

export class GlfwVulkanSurfaceProvider final : public IVulkanSurfaceProvider {
  public:
    explicit GlfwVulkanSurfaceProvider(IWindowSystem* WindowSys);

    [[nodiscard]] auto GetRequiredInstanceExtensions() const
        -> std::expected<std::vector<const char*>, ErrorMessage> override;

    [[nodiscard]] auto CreateSurface(vk::raii::Instance& Instance)
        -> std::expected<vk::raii::SurfaceKHR, ErrorMessage> override;

    [[nodiscard]] auto GetFramebufferExtent() const -> FramebufferExtent override;

  private:
    GlfwWindowSystem* m_WindowSystem = nullptr;
    GLFWwindow*       m_Window       = nullptr;
};

/// @brief Create the Vulkan surface adapter for @p WindowSys.
export [[nodiscard]] auto CreateVulkanSurfaceProvider(IWindowSystem* WindowSys)
    -> std::expected<UPtr<IVulkanSurfaceProvider>, ErrorMessage>;

} // namespace SoulEngine
