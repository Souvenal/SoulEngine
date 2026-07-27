module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <magic_enum/magic_enum.hpp>

export module Vulkan:SurfaceProvider;

export import WindowSystem;
import RHI;
import vulkan;
import std;

namespace SoulEngine {

/// @brief Creates a Vulkan presentation surface for one window-system implementation.
class IVulkanSurfaceProvider {
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

class GlfwVulkanSurfaceProvider final : public IVulkanSurfaceProvider {
  public:
    explicit GlfwVulkanSurfaceProvider(IWindowSystem* WindowSys) {
        m_WindowSystem = static_cast<GlfwWindowSystem*>(WindowSys);
        m_Window       = m_WindowSystem->GetNativeHandle();
    }

    [[nodiscard]] auto GetRequiredInstanceExtensions() const
        -> std::expected<std::vector<const char*>, ErrorMessage> override {
        uint32_t    Count      = 0;
        const char** Extensions = glfwGetRequiredInstanceExtensions(&Count);
        if (!Extensions) {
            const char* Desc = nullptr;
            glfwGetError(&Desc);
            return std::unexpected(ErrorMessage(Format("glfwGetRequiredInstanceExtensions failed: {}",
                                                        Desc ? Desc : "GLFW not initialized or no Vulkan support")));
        }
        return std::vector<const char*>(Extensions, Extensions + Count);
    }

    [[nodiscard]] auto CreateSurface(vk::raii::Instance& Instance)
        -> std::expected<vk::raii::SurfaceKHR, ErrorMessage> override {
        VkSurfaceKHR RawSurface = VK_NULL_HANDLE;
        VkResult Result = glfwCreateWindowSurface(static_cast<VkInstance>(*Instance), m_Window, nullptr, &RawSurface);
        if (Result != VK_SUCCESS) {
            const char* Desc = nullptr;
            glfwGetError(&Desc);
            return std::unexpected(ErrorMessage(Format("glfwCreateWindowSurface failed ({}): {}",
                                                        vk::to_string(static_cast<vk::Result>(Result)),
                                                        Desc ? Desc : "unknown error")));
        }
        return vk::raii::SurfaceKHR(Instance, RawSurface);
    }

    [[nodiscard]] auto GetFramebufferExtent() const -> FramebufferExtent override {
        return m_WindowSystem->GetFramebufferExtent();
    }

  private:
    GlfwWindowSystem* m_WindowSystem = nullptr;
    GLFWwindow*       m_Window       = nullptr;
};

/// @brief Create the Vulkan surface adapter for @p WindowSys.
[[nodiscard]] auto CreateVulkanSurfaceProvider(IWindowSystem* WindowSys)
    -> std::expected<UPtr<IVulkanSurfaceProvider>, ErrorMessage> {
    if (!WindowSys || !WindowSys->IsValid())
        return std::unexpected(ErrorMessage("Window system is invalid — off-screen rendering is not supported"));

    switch (WindowSys->GetType()) {
    case WindowSystemType::Glfw:
        return std::make_unique<GlfwVulkanSurfaceProvider>(WindowSys);
    case WindowSystemType::Unknown:
        return std::unexpected(ErrorMessage("Window system type is unknown"));
    default:
        return std::unexpected(ErrorMessage(Format("Vulkan backend does not support window system type '{}' yet",
                                                    magic_enum::enum_name(WindowSys->GetType()))));
    }
}

} // namespace SoulEngine
