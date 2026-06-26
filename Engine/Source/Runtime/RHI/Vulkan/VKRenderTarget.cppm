module;

#include <vk_mem_alloc.h>

export module Vulkan:RenderTarget;

import Core;
import vulkan;
import RHI;
import std;

import :Types;
import :DeletionQueue;
import :Texture;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

/// Vulkan render target — wraps DeviceTexture for color/depth attachment usage.
/// Color vs depth/stencil is determined by format and usage in the desc.
/// No bindless descriptor slot needed — accessed via attachment image views.
class RenderTarget final : public RHI::RenderTarget {
  public:
    RenderTarget(SPtr<DeviceTexture> Tex, Uint32 Width, Uint32 Height, RHI::Format Fmt, RHI::TextureUsage Usage)
        : m_Texture(std::move(Tex)), m_Width(Width), m_Height(Height), m_Format(Fmt), m_Usage(Usage) {}

    ~RenderTarget() override = default;

    RenderTarget(const RenderTarget&)                    = delete;
    auto operator=(const RenderTarget&) -> RenderTarget& = delete;
    RenderTarget(RenderTarget&&)                         = delete;
    auto operator=(RenderTarget&&) -> RenderTarget&      = delete;

    [[nodiscard]] static auto
    Create(const RHI::RenderTargetDesc& Desc, VmaAllocator Alloc, vk::raii::Device& Dev, DeletionQueue& DelQueue)
        -> std::expected<RHI::RenderTargetCreateResult, ErrorMessage> {

        if (Desc.Width == 0 || Desc.Height == 0)
            return std::unexpected(ErrorMessage("RenderTarget::Create: invalid dimensions (zero)"));

        vk::Format           VkFmt  = ToVkFormat(Desc.Format);
        vk::ImageUsageFlags  Usage  = ResolveUsageFlags(Desc.Usage);
        vk::ImageAspectFlags Aspect = ToVkImageAspect(Desc.Format);

        // ── Create VkImage ─────────────────────────────────────────────
        vk::ImageCreateInfo ImageCI{
            .imageType     = vk::ImageType::e2D,
            .format        = VkFmt,
            .extent        = {Desc.Width, Desc.Height, 1},
            .mipLevels     = 1,
            .arrayLayers   = 1,
            .samples       = vk::SampleCountFlagBits::e1,
            .tiling        = vk::ImageTiling::eOptimal,
            .usage         = Usage,
            .sharingMode   = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };

        VmaAllocationCreateInfo ImageAllocInfo{
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        };

        VkImage           RawImage = nullptr;
        VmaAllocation     RawAlloc = nullptr;
        VkImageCreateInfo RawCI    = static_cast<VkImageCreateInfo>(ImageCI);
        if (vmaCreateImage(Alloc, &RawCI, &ImageAllocInfo, &RawImage, &RawAlloc, nullptr) != VK_SUCCESS)
            return std::unexpected(ErrorMessage("RenderTarget::Create: vmaCreateImage failed"));

        auto VkImage = static_cast<vk::Image>(RawImage);

        // ── Create ImageView ───────────────────────────────────────────
        vk::ImageViewCreateInfo ViewCI{
            .image      = VkImage,
            .viewType   = vk::ImageViewType::e2D,
            .format     = VkFmt,
            .components = {vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity},
            .subresourceRange =
                {.aspectMask = Aspect, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
        };
        auto ViewRes = Dev.createImageView(ViewCI);
        if (ViewRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("RenderTarget::Create: vkCreateImageView failed"));

        // Wrap in DeviceTexture (DescManager = nullptr, DescriptorSlot = 0 —
        // not sampled via bindless descriptors; no slot to free on destroy).
        auto Tex = std::make_shared<DeviceTexture>(Alloc, VkImage, RawAlloc, std::move(ViewRes.value), nullptr, 0);

        return RHI::RenderTargetCreateResult{
            .Texture = std::make_shared<RenderTarget>(std::move(Tex), Desc.Width, Desc.Height, Desc.Format, Desc.Usage),
        };
    }

    // ── RHI::RenderTarget interface ──────────────────────────────────
    [[nodiscard]] auto GetWidth() const -> Uint32 override {
        return m_Width;
    }
    [[nodiscard]] auto GetHeight() const -> Uint32 override {
        return m_Height;
    }
    [[nodiscard]] auto GetFormat() const -> RHI::Format override {
        return m_Format;
    }
    [[nodiscard]] auto GetUsage() const -> RHI::TextureUsage override {
        return m_Usage;
    }

    // ── Vulkan-specific downcast accessors for BeginPass ────────────
    [[nodiscard]] auto GetVkImage() const -> vk::Image {
        return m_Texture->GetImage();
    }
    [[nodiscard]] auto GetVkImageView() const -> vk::ImageView {
        return m_Texture->GetImageView();
    }

  private:
    [[nodiscard]] static auto ResolveUsageFlags(RHI::TextureUsage Usage) -> vk::ImageUsageFlags {
        vk::ImageUsageFlags Flags = vk::ImageUsageFlagBits::eTransferDst;
        if ((static_cast<Uint32>(Usage) & static_cast<Uint32>(RHI::TextureUsage::RenderTarget)) != 0)
            Flags |= vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled;
        if ((static_cast<Uint32>(Usage) & static_cast<Uint32>(RHI::TextureUsage::DepthStencil)) != 0)
            Flags |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        return Flags;
    }

    SPtr<DeviceTexture> m_Texture = nullptr;
    Uint32              m_Width   = 0;
    Uint32              m_Height  = 0;
    RHI::Format         m_Format  = RHI::Format::Unknown;
    RHI::TextureUsage   m_Usage   = RHI::TextureUsage::None;
};

} // namespace SoulEngine::RHI::Vulkan
