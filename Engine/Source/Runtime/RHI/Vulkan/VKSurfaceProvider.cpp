module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

module Vulkan:SurfaceProvider.impl;

import magic_enum;
import WindowSystem;
import RHI;
import vulkan;
import std;
import Core;

import :SurfaceProvider;

namespace SoulEngine {

GlfwVulkanSurfaceProvider::GlfwVulkanSurfaceProvider(IWindowSystem* WindowSys) {
    m_WindowSystem = static_cast<GlfwWindowSystem*>(WindowSys);
    m_Window       = m_WindowSystem->GetNativeHandle();
}

auto GlfwVulkanSurfaceProvider::GetRequiredInstanceExtensions() const
    -> std::expected<std::vector<const char*>, ErrorMessage> {
    uint32_t     Count      = 0;
    const char** Extensions = glfwGetRequiredInstanceExtensions(&Count);
    if (!Extensions) {
        const char* Desc = nullptr;
        glfwGetError(&Desc);
        return std::unexpected(ErrorMessage(Format("glfwGetRequiredInstanceExtensions failed: {}",
                                                   Desc ? Desc : "GLFW not initialized or no Vulkan support")));
    }
    return std::vector<const char*>(Extensions, Extensions + Count);
}

auto GlfwVulkanSurfaceProvider::CreateSurface(vk::raii::Instance& Instance)
    -> std::expected<vk::raii::SurfaceKHR, ErrorMessage> {
    VkSurfaceKHR RawSurface = VK_NULL_HANDLE;
    VkResult     Result = glfwCreateWindowSurface(static_cast<VkInstance>(*Instance), m_Window, nullptr, &RawSurface);
    if (Result != VK_SUCCESS) {
        const char* Desc = nullptr;
        glfwGetError(&Desc);
        return std::unexpected(ErrorMessage(Format("glfwCreateWindowSurface failed ({}): {}",
                                                   vk::to_string(static_cast<vk::Result>(Result)),
                                                   Desc ? Desc : "unknown error")));
    }
    return vk::raii::SurfaceKHR(Instance, RawSurface);
}

auto GlfwVulkanSurfaceProvider::GetFramebufferExtent() const -> FramebufferExtent {
    return m_WindowSystem->GetFramebufferExtent();
}

auto CreateVulkanSurfaceProvider(IWindowSystem* WindowSys)
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
