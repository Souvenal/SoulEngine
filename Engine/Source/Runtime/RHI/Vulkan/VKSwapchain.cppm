module;

// Clang 22 named-modules bug: bare uint32_t is shadowed through the std re-export chain (import RHI); a
// textual include restores it.
#include <cstdint>

export module Vulkan:Swapchain;

// import std;
import RHI;

import vulkan;

import :Debug;
import :Context;
import :SurfaceProvider;
import Core;
import WindowSystem;

namespace SoulEngine {

[[nodiscard]] auto ResolveSurfaceFormat(std::span<const vk::SurfaceFormatKHR> Formats) -> vk::SurfaceFormatKHR;

[[nodiscard]] auto ResolvePresentMode(std::span<const vk::PresentModeKHR> Modes) -> vk::PresentModeKHR;

[[nodiscard]] auto ResolveExtent(IVulkanSurfaceProvider& Provider, const vk::SurfaceCapabilitiesKHR& Caps)
    -> vk::Extent2D;

[[nodiscard]] auto ResolveSwapImageCount(const vk::SurfaceCapabilitiesKHR& Caps) -> uint32_t;

// VulkanSwapchain
//
// Owns the VkSwapchainKHR and its image views. Does NOT own the device, physical device, or surface — those
// are held by VulkanRenderDevice.
//
// VulkanSwapchain operations (AcquireNextImage, Present) are not thread-safe and must be serialized by the
// caller (typically the main thread).

class VulkanSwapchain {
  public:
    VulkanSwapchain() = default;

    /// @brief Create a fully initialized VulkanSwapchain.
    ///
    /// Stores a non-owning reference to Vulkan objects owned by VulkanResourceContext.
    [[nodiscard]] static auto Create(const VulkanResourceContext& Context)
        -> std::expected<VulkanSwapchain, ErrorMessage>;

    auto Cleanup() -> void;

    /// Recreate the swapchain (e.g. after window resize or out-of-date).
    [[nodiscard]] auto Recreate() -> std::expected<void, ErrorMessage>;
    // Per-frame operations

    [[nodiscard]] auto AcquireNextImage(vk::Semaphore PresentCompleteSema) -> vk::Result;

    [[nodiscard]] auto Present(vk::raii::Queue& PresentQueue) -> vk::Result;
    // Accessors

    [[nodiscard]] auto GetExtent() const -> vk::Extent2D;
    [[nodiscard]] auto GetFormat() const -> vk::SurfaceFormatKHR;
    [[nodiscard]] auto GetCurrentIndex() const -> uint32_t;
    [[nodiscard]] auto GetImageCount() const -> uint32_t;

    [[nodiscard]] auto GetImage(uint32_t Index) const -> vk::Image;

    [[nodiscard]] auto GetCurrentImageView() const -> vk::ImageView;

    /// @brief The render-complete semaphore for the currently acquired swapchain image.
    ///
    /// Used by VulkanRenderDevice::EndFrame to build the semaphore-submit info for queue submit.
    [[nodiscard]] auto GetCurrentRenderCompleteSemaphore() const -> vk::Semaphore;

    // Non-owning pointers (set in Create)
  private:
    const VulkanResourceContext* m_Context      = nullptr;
    vk::raii::Device*            m_Device       = nullptr;
    vk::raii::PhysicalDevice*    m_PhysDevice   = nullptr;
    vk::raii::SurfaceKHR*        m_Surface      = nullptr;
    VulkanDebugUtils*            m_DebugUtils   = nullptr;
    VulkanImageTracker*          m_ImageTracker = nullptr;
    // Owned resources

    vk::raii::SwapchainKHR           m_Swapchain = nullptr;
    vk::SurfaceFormatKHR             m_Format    = {};
    vk::Extent2D                     m_Extent    = {1, 1};
    std::vector<vk::Image>           m_Images;
    std::vector<vk::raii::ImageView> m_ImageViews;
    std::vector<vk::raii::Semaphore> m_RenderComplete;
    uint32_t                         m_CurrentIndex = 0;
};

} // namespace SoulEngine
