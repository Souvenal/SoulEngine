module;

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

export module Vulkan:Swapchain;

import RHI;

import std;
import vulkan;

using namespace SoulEngine;
using namespace SoulEngine::Core;
using namespace SoulEngine::RHI;

namespace SoulEngine::RHI::Vulkan {

[[nodiscard]] auto ResolveSurfaceFormat(std::span<const vk::SurfaceFormatKHR> Formats) -> vk::SurfaceFormatKHR {
    for (auto& Fmt : Formats)
        if (Fmt.format == vk::Format::eB8G8R8A8Unorm && Fmt.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear)
            return Fmt;
    return Formats[0];
}

[[nodiscard]] auto ResolvePresentMode(std::span<const vk::PresentModeKHR> Modes) -> vk::PresentModeKHR {
    // Vulkan present modes:
    //
    // eImmediate    — frames are pushed to the display immediately; will tear.
    //
    // eFifo         — swapchain is a queue.  Display pops from the front at
    //                 vblank; application pushes to the back.  Blocks when full.
    //                 Equivalent to classic vsync.  Always available.
    //
    // eFifoRelaxed  — like eFifo, but if the app was late and the queue is
    //                 empty at vblank, the image is transferred immediately upon
    //                 arrival instead of waiting for the next vblank.  May tear.
    //
    // eMailbox      — like eFifo, but when the queue is full, new frames replace
    //                 the oldest queued frame instead of blocking.  Tear-free
    //                 while letting the app render as fast as possible ("triple
    //                 buffering").
    //
    // Prefer mailbox for low-latency tear-free presentation; fall back to fifo.
    for (auto& Mode : Modes)
        if (Mode == vk::PresentModeKHR::eMailbox)
            return Mode;
    return vk::PresentModeKHR::eFifo;
}

[[nodiscard]] auto ResolveExtent(GLFWwindow* Window, const vk::SurfaceCapabilitiesKHR& Caps) -> vk::Extent2D {
    // currentExtent is the window size in pixels, clamped to surface limits.
    // When it equals UINT32_MAX the surface doesn't dictate a size (e.g. some
    // compositors) — we must query the framebuffer dimensions.
    //
    // GLFW has two coordinate systems:
    //
    //   glfwGetWindowSize()      → screen coordinates (e.g. 1280×720)
    //   glfwGetFramebufferSize() → physical pixels  (e.g. 2560×1440 on Retina)
    //
    // On high-DPI displays, screen coordinates and physical pixels differ because
    // each screen point maps to multiple pixels (e.g. 2× on Retina).
    //
    // Vulkan works in pixels.  So we use the framebuffer pixel size.
    if (Caps.currentExtent.width != std::numeric_limits<Uint32>::max())
        return Caps.currentExtent;

    int Width = 0, Height = 0;
    glfwGetFramebufferSize(Window, &Width, &Height);

    return vk::Extent2D{
        .width  = std::clamp(static_cast<uint32_t>(Width), Caps.minImageExtent.width, Caps.maxImageExtent.width),
        .height = std::clamp(static_cast<uint32_t>(Height), Caps.minImageExtent.height, Caps.maxImageExtent.height),
    };
}

[[nodiscard]] auto ResolveSwapImageCount(const vk::SurfaceCapabilitiesKHR& Caps) -> uint32_t {
    // Request at least one more than the surface minimum so the driver
    // doesn't have to stall internally while we acquire the next image.
    //
    // maxImageCount == 0 means the implementation has no upper bound.
    uint32_t Count = std::max(Caps.minImageCount + 1, 3u);

    if (Caps.maxImageCount > 0 && Count > Caps.maxImageCount)
        Count = Caps.maxImageCount;

    return Count;
}

// ═════════════════════════════════════════════════════════════════════════════
// Swapchain
// ═════════════════════════════════════════════════════════════════════════════
//
// Owns the VkSwapchainKHR and its image views.  Does NOT own the device,
// physical device, or surface — those are held by RenderDevice.
//
// Swapchain operations (AcquireNextImage, Present) are not thread-safe
// and must be serialized by the caller (typically the main thread).

class Swapchain {
  public:
    Swapchain() = default;

    /// Create a fully initialized Swapchain. Stores non-owning references to
    /// Vulkan objects owned by RenderDevice.
    [[nodiscard]] static auto Create(vk::raii::Device&         Device,
                                     vk::raii::PhysicalDevice& PhysDevice,
                                     vk::raii::SurfaceKHR&     Surface,
                                     GLFWwindow*               Window) -> std::expected<Swapchain, ErrorMessage> {
        Swapchain Result;
        Result.m_Device     = &Device;
        Result.m_PhysDevice = &PhysDevice;
        Result.m_Surface    = &Surface;
        Result.m_Window     = Window;

        // ── Query surface capabilities ──────────────────────────────────
        auto CapsResult = Result.m_PhysDevice->getSurfaceCapabilitiesKHR(*Result.m_Surface);
        if (CapsResult.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Failed to query surface capabilities"));

        auto FormatsResult = Result.m_PhysDevice->getSurfaceFormatsKHR(*Result.m_Surface);
        if (FormatsResult.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Failed to query surface formats"));

        auto PresentResult = Result.m_PhysDevice->getSurfacePresentModesKHR(*Result.m_Surface);
        if (PresentResult.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Failed to query surface present modes"));

        auto& Caps         = CapsResult.value;
        auto& Formats      = FormatsResult.value;
        auto& PresentModes = PresentResult.value;

        // ── Choose surface format and present mode ────────────────────
        vk::SurfaceFormatKHR ChosenFormat = ResolveSurfaceFormat(Formats);
        Result.m_Format                   = ChosenFormat;
        vk::PresentModeKHR PresentMode    = ResolvePresentMode(PresentModes);

        // ── Resolve extent ────────────────────────────────────────────
        Result.m_Extent = ResolveExtent(Result.m_Window, Caps);

        // ── Resolve image count ────────────────────────────────────────
        uint32_t ImageCount = ResolveSwapImageCount(Caps);

        LogInfo("Surface format: {}, color space: {}, present mode: {}, extent: {}x{}, image count: {}",
                vk::to_string(ChosenFormat.format),
                vk::to_string(ChosenFormat.colorSpace),
                vk::to_string(PresentMode),
                Result.m_Extent.width,
                Result.m_Extent.height,
                ImageCount);

        if ((Caps.supportedUsageFlags & vk::ImageUsageFlagBits::eTransferDst) == vk::ImageUsageFlags{})
            return std::unexpected(ErrorMessage("Swapchain does not support transfer-destination presentation"));

        // ── Create swapchain ────────────────────────────────────────────
        vk::SwapchainCreateInfoKHR SwapchainCI{
            .surface          = *Result.m_Surface,
            .minImageCount    = ImageCount,
            .imageFormat      = ChosenFormat.format,
            .imageColorSpace  = ChosenFormat.colorSpace,
            .imageExtent      = Result.m_Extent,
            // specifies the number of layers each image consists of,
            // always 1 unless developing for stereoscopic 3D applications.
            .imageArrayLayers = 1,
            // The engine renders to its own color RT and blits that output to
            // swapchain; render passes must not use swapchain images directly.
            .imageUsage       = vk::ImageUsageFlagBits::eTransferDst,
            // eExclusive means that an image is owned by one queue family at a time,
            // and ownership must be explicitly transferred before using the image in another queue family.
            //
            // eConcurrent means that images can be used across multiple queue families,
            // without explicit ownership transfers.
            //
            // Concurrent is a complex topic, we'll extend to it in the future.
            .imageSharingMode = vk::SharingMode::eExclusive,
            // we can do a 90-degree clockwise rotation or horizontal flip, or keep it as-is.
            .preTransform     = Caps.currentTransform,
            // Specifies if the alpha channel should be used for blending with other windows
            // almost always eOpaque for games.
            .compositeAlpha   = vk::CompositeAlphaFlagBitsKHR::eOpaque,
            .presentMode      = PresentMode,
            // When clipped is true, it means we don't care about the pixels that are obscured
            // (e.g. by another window or off-screen) and the implementation can optimize for that.
            // Unless we want to read back the pixels, we can enable it for better performance.
            .clipped          = vk::True,
            // for swapchain recreation; set to the current swapchain handle when recreating
            // recreation is a future topic, so we set it to nullptr for now
            .oldSwapchain     = nullptr,
        };

        auto [Res, SC] = Result.m_Device->createSwapchainKHR(SwapchainCI);
        if (Res != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage(Core::Format("Failed to create swapchain: {}", vk::to_string(Res))));
        Result.m_Swapchain = std::move(SC);

        // ── Retrieve swapchain images ───────────────────────────────────
        auto ImagesResult = Result.m_Swapchain.getImages();
        if (ImagesResult.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage(
                Core::Format("Failed to retrieve swapchain images: {}", vk::to_string(ImagesResult.result))));
        Result.m_Images = std::move(ImagesResult.value);

        // ── Create render-complete binary semaphores (one per swapchain image) ──
        Result.m_RenderComplete.reserve(Result.m_Images.size());
        for (size_t i = 0; i < Result.m_Images.size(); ++i) {
            auto SemRes = Result.m_Device->createSemaphore({});
            if (SemRes.result != vk::Result::eSuccess)
                return std::unexpected(ErrorMessage("Failed to create render-complete semaphore"));
            Result.m_RenderComplete.emplace_back(std::move(SemRes.value));
        }

        LogInfo("Swapchain created: {}x{}, format={}, images={}",
                Result.m_Extent.width,
                Result.m_Extent.height,
                vk::to_string(Result.m_Format.format),
                static_cast<uint32_t>(Result.m_Images.size()));

        return Result;
    }

    auto Cleanup() -> void {
        m_Images.clear();
        m_RenderComplete.clear();
        m_Swapchain    = nullptr;
        m_CurrentIndex = 0;
    }

    /// Recreate the swapchain (e.g. after window resize or out-of-date).
    [[nodiscard]] auto Recreate() -> std::expected<void, ErrorMessage> {
        m_Device->waitIdle();
        Cleanup();
        auto NewSc = Create(*m_Device, *m_PhysDevice, *m_Surface, m_Window);
        if (!NewSc)
            return std::unexpected(NewSc.error());
        *this = std::move(*NewSc);
        return {};
    }

    // ── Per-frame operations ─────────────────────────────────────────────

    [[nodiscard]] auto AcquireNextImage(vk::Semaphore PresentCompleteSema) -> vk::Result {
        // Note: the image index value is return immediatedly, but,
        //       the `signal semaphore` event is gpu aync.
        //       The guarantee is that once the image is used up for presentation,
        //       the chosen semaphore will be signaled.
        auto [Res, Index] =
            m_Swapchain.acquireNextImage(std::numeric_limits<Uint64>::max(), PresentCompleteSema, nullptr);

        if (Res == vk::Result::eSuccess || Res == vk::Result::eSuboptimalKHR)
            m_CurrentIndex = Index;

        return Res;
    }

    [[nodiscard]] auto Present(vk::raii::Queue& PresentQueue) -> vk::Result {
        uint32_t           ImageIndex = GetCurrentIndex();
        vk::SwapchainKHR   RawSC      = *m_Swapchain;
        vk::Semaphore      RenderDone = *m_RenderComplete[ImageIndex];
        vk::PresentInfoKHR PresentInfo{
            .waitSemaphoreCount = 1,
            .pWaitSemaphores    = &RenderDone,
            .swapchainCount     = 1,
            .pSwapchains        = &RawSC,
            .pImageIndices      = &m_CurrentIndex,
        };

        return PresentQueue.presentKHR(PresentInfo);
    }

    // ── Accessors ────────────────────────────────────────────────────────

    [[nodiscard]] auto GetExtent() const -> vk::Extent2D {
        return m_Extent;
    }
    [[nodiscard]] auto GetFormat() const -> vk::SurfaceFormatKHR {
        return m_Format;
    }
    [[nodiscard]] auto GetCurrentIndex() const -> uint32_t {
        return m_CurrentIndex;
    }
    [[nodiscard]] auto GetImageCount() const -> uint32_t {
        return static_cast<uint32_t>(m_Images.size());
    }

    [[nodiscard]] auto GetImage(uint32_t Index) const -> vk::Image {
        return (Index < m_Images.size()) ? m_Images[Index] : vk::Image{};
    }

    /// Returns the render-complete semaphore for the currently acquired
    /// swapchain image.  Used by RenderDevice::EndFrame to build the
    /// semaphore-submit info for queue submit.
    [[nodiscard]] auto GetCurrentRenderCompleteSemaphore() const -> vk::Semaphore {
        return *m_RenderComplete[m_CurrentIndex];
    }

  private:
    // ── Non-owning pointers (set in Create) ─────────────────────────────

    vk::raii::Device*         m_Device     = nullptr;
    vk::raii::PhysicalDevice* m_PhysDevice = nullptr;
    vk::raii::SurfaceKHR*     m_Surface    = nullptr;
    GLFWwindow*               m_Window     = nullptr;

    // ── Owned resources ──────────────────────────────────────────────────

    vk::raii::SwapchainKHR           m_Swapchain = nullptr;
    vk::SurfaceFormatKHR             m_Format    = {};
    vk::Extent2D                     m_Extent    = {1, 1};
    std::vector<vk::Image>           m_Images;
    std::vector<vk::raii::Semaphore> m_RenderComplete;
    uint32_t                         m_CurrentIndex = 0;
};

} // namespace SoulEngine::RHI::Vulkan
