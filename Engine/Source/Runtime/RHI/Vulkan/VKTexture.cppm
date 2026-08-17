module;

#include <vk_mem_alloc.h>

export module Vulkan:Texture;

import Core;
import vulkan;
import RHI;
import std;

import :Types;
import :Buffer;
import :Context;

namespace SoulEngine {

// ═════════════════════════════════════════════════════════════════════════════
// VulkanDeviceTexture — reusable GPU image wrapper
// ═════════════════════════════════════════════════════════════════════════════

/// Owns VkImage + VkImageView + VMA allocation lifecycle.
class VulkanDeviceTexture {
  public:
    VulkanDeviceTexture() = default;

    VulkanDeviceTexture(VmaAllocator Alloc, vk::Image Image, VmaAllocation Allocation, vk::raii::ImageView&& ImageView)
        : m_Allocator(Alloc), m_Image(Image), m_Allocation(Allocation), m_ImageView(std::move(ImageView)) {}

    ~VulkanDeviceTexture() {
        if (m_Allocation)
            vmaDestroyImage(m_Allocator, static_cast<VkImage>(m_Image), m_Allocation);
    }

    // Move-only.
    VulkanDeviceTexture(VulkanDeviceTexture&& Other) noexcept
        : m_Allocator(std::exchange(Other.m_Allocator, nullptr)),
          m_Image(std::exchange(Other.m_Image, nullptr)),
          m_Allocation(std::exchange(Other.m_Allocation, nullptr)),
          m_ImageView(std::move(Other.m_ImageView)) {}

    auto operator=(VulkanDeviceTexture&& Other) noexcept -> VulkanDeviceTexture& {
        if (this != &Other) {
            std::swap(m_Allocator, Other.m_Allocator);
            std::swap(m_Image, Other.m_Image);
            std::swap(m_Allocation, Other.m_Allocation);
            std::swap(m_ImageView, Other.m_ImageView);
        }
        return *this;
    }

    VulkanDeviceTexture(const VulkanDeviceTexture&) = delete;
    auto operator=(const VulkanDeviceTexture&)      = delete;

    [[nodiscard]] auto GetImage() const -> vk::Image {
        return m_Image;
    }
    [[nodiscard]] auto GetImageView() const -> vk::ImageView {
        return *m_ImageView;
    }
    [[nodiscard]] auto GetAllocation() const -> VmaAllocation {
        return m_Allocation;
    }

    /// Submit a staging-buffer → image copy via VulkanImmediateContext.
    /// Handles Undefined→TransferDst→ShaderReadOnly barriers.
    /// Returns upload completion token (caller defers staging destruction).
    [[nodiscard]] auto CopyFrom(VulkanHostBuffer&                      Staging,
                                VulkanImmediateContext&                Ctx,
                                Uint32                                 Width,
                                Uint32                                 Height,
                                vk::Format                             VkFmt,
                                VulkanImmediateContext::CompletionDesc Completion)
        -> std::expected<void, ErrorMessage> {
        return Ctx.Submit(
            VulkanImmediateQueue::Transfer,
            vk::PipelineStageFlagBits2::eTransfer,
            [&](const vk::raii::CommandBuffer& CmdBuf) {
                // Barrier: Undefined → TransferDst
                vk::ImageMemoryBarrier2 Barrier1{
                    .srcStageMask        = vk::PipelineStageFlagBits2::eNone,
                    .srcAccessMask       = vk::AccessFlagBits2::eNone,
                    .dstStageMask        = vk::PipelineStageFlagBits2::eTransfer,
                    .dstAccessMask       = vk::AccessFlagBits2::eTransferWrite,
                    .oldLayout           = vk::ImageLayout::eUndefined,
                    .newLayout           = vk::ImageLayout::eTransferDstOptimal,
                    .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                    .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                    .image               = m_Image,
                    .subresourceRange    = {.aspectMask     = vk::ImageAspectFlagBits::eColor,
                                            .baseMipLevel   = 0,
                                            .levelCount     = 1,
                                            .baseArrayLayer = 0,
                                            .layerCount     = 1},
                };
                CmdBuf.pipelineBarrier2(
                    vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &Barrier1});
                // Copy buffer → image
                vk::BufferImageCopy Region{
                    .bufferOffset      = 0,
                    .bufferRowLength   = 0,
                    .bufferImageHeight = 0,
                    .imageSubresource  = {.aspectMask     = vk::ImageAspectFlagBits::eColor,
                                          .mipLevel       = 0,
                                          .baseArrayLayer = 0,
                                          .layerCount     = 1},
                    .imageOffset       = {0, 0, 0},
                    .imageExtent       = {Width, Height, 1},
                };
                CmdBuf.copyBufferToImage(Staging.Get(), m_Image, vk::ImageLayout::eTransferDstOptimal, {Region});
                // Barrier: TransferDst → ShaderReadOnly
                vk::ImageMemoryBarrier2 Barrier2{
                    .srcStageMask        = vk::PipelineStageFlagBits2::eTransfer,
                    .srcAccessMask       = vk::AccessFlagBits2::eTransferWrite,
                    .dstStageMask        = vk::PipelineStageFlagBits2::eNone,
                    .dstAccessMask       = vk::AccessFlagBits2::eNone,
                    .oldLayout           = vk::ImageLayout::eTransferDstOptimal,
                    .newLayout           = vk::ImageLayout::eShaderReadOnlyOptimal,
                    .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                    .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                    .image               = m_Image,
                    .subresourceRange    = {.aspectMask     = vk::ImageAspectFlagBits::eColor,
                                            .baseMipLevel   = 0,
                                            .levelCount     = 1,
                                            .baseArrayLayer = 0,
                                            .layerCount     = 1},
                };
                CmdBuf.pipelineBarrier2(
                    vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &Barrier2});
            },
            std::move(Completion));
    }

  private:
    VmaAllocator        m_Allocator  = nullptr;
    vk::Image           m_Image      = nullptr;
    VmaAllocation       m_Allocation = nullptr;
    vk::raii::ImageView m_ImageView  = nullptr;
};

// ═════════════════════════════════════════════════════════════════════════════
// VulkanSampledTexture — GPU sampled texture resource
// ═════════════════════════════════════════════════════════════════════════════

class VulkanSampledTexture final : public RHISampledTexture {
  public:
    VulkanSampledTexture(SPtr<VulkanDeviceTexture> Tex, Uint32 Width, Uint32 Height)
        : m_Texture(std::move(Tex)), m_Width(Width), m_Height(Height) {}

    ~VulkanSampledTexture() override = default;

    VulkanSampledTexture(const VulkanSampledTexture&)                    = delete;
    auto operator=(const VulkanSampledTexture&) -> VulkanSampledTexture& = delete;
    VulkanSampledTexture(VulkanSampledTexture&&)                         = delete;
    auto operator=(VulkanSampledTexture&&) -> VulkanSampledTexture&      = delete;

    /// Static factory: upload pixel data to GPU texture via staging buffer.
    [[nodiscard]] static auto Create(const VulkanResourceContext&         Context,
                                     const RHISampledTextureDesc&         Desc,
                                     VulkanImmediateContext::CompletionFn OnReady)
        -> std::expected<UPtr<VulkanSampledTexture>, ErrorMessage> {
        if (Desc.Data.empty() || Desc.Width == 0 || Desc.Height == 0 || Desc.Channels == 0)
            return std::unexpected(
                ErrorMessage("VulkanSampledTexture::Create: invalid desc (empty data or zero dimensions)"));
        if (Desc.Width > std::numeric_limits<Uint64>::max() / Desc.Height)
            return std::unexpected(ErrorMessage("VulkanSampledTexture::Create: pixel count overflows Uint64"));
        const Uint64 PixelCount = static_cast<Uint64>(Desc.Width) * Desc.Height;
        if (PixelCount > std::numeric_limits<Uint64>::max() / Desc.Channels)
            return std::unexpected(ErrorMessage("VulkanSampledTexture::Create: source data size overflows Uint64"));
        const Uint64 PixelSize = PixelCount * Desc.Channels;
        if (Desc.Data.size_bytes() != PixelSize)
            return std::unexpected(
                ErrorMessage("VulkanSampledTexture::Create: data size does not match texture dimensions"));

        auto StagingRes = VulkanHostBuffer::Create(
            PixelSize, vk::BufferUsageFlagBits::eTransferSrc, Context.Device, Context.Allocator);
        if (!StagingRes)
            return std::unexpected(StagingRes.error().Append("VulkanSampledTexture::Create: staging creation failed"));
        auto Staging = std::make_shared<VulkanHostBuffer>(std::move(*StagingRes));
        if (auto R = Staging->Upload(Desc.Data.data(), PixelSize); !R)
            return std::unexpected(R.error().Append("VulkanSampledTexture::Create: staging upload failed"));

        const auto VkFmt = ToVkFormat(Desc.Format);
        if (VkFmt == vk::Format::eUndefined)
            return std::unexpected(ErrorMessage("VulkanSampledTexture::Create: unsupported texture format"));
        const std::array    QueueFamilies{Context.GraphicsFamily, Context.TransferFamily};
        const bool          bConcurrentSharing = Context.GraphicsFamily != Context.TransferFamily;
        vk::ImageCreateInfo ImageCI{
            .imageType             = vk::ImageType::e2D,
            .format                = VkFmt,
            .extent                = {Desc.Width, Desc.Height, 1},
            .mipLevels             = 1,
            .arrayLayers           = 1,
            .samples               = vk::SampleCountFlagBits::e1,
            .tiling                = vk::ImageTiling::eOptimal,
            .usage                 = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
            .sharingMode           = bConcurrentSharing ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive,
            .queueFamilyIndexCount = bConcurrentSharing ? static_cast<Uint32>(QueueFamilies.size()) : 0,
            .pQueueFamilyIndices   = bConcurrentSharing ? QueueFamilies.data() : nullptr,
            .initialLayout         = vk::ImageLayout::eUndefined,
        };
        VmaAllocationCreateInfo ImageAllocInfo{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkImage                 RawImage = nullptr;
        VmaAllocation           RawAlloc = nullptr;
        VkImageCreateInfo       RawCI    = static_cast<VkImageCreateInfo>(ImageCI);
        if (vmaCreateImage(Context.Allocator, &RawCI, &ImageAllocInfo, &RawImage, &RawAlloc, nullptr) != VK_SUCCESS)
            return std::unexpected(ErrorMessage("VulkanSampledTexture::Create: vmaCreateImage failed"));
        const auto              VkImage = static_cast<vk::Image>(RawImage);
        vk::ImageViewCreateInfo ViewCI{
            .image            = VkImage,
            .viewType         = vk::ImageViewType::e2D,
            .format           = VkFmt,
            .components       = {vk::ComponentSwizzle::eIdentity,
                                 vk::ComponentSwizzle::eIdentity,
                                 vk::ComponentSwizzle::eIdentity,
                                 vk::ComponentSwizzle::eIdentity},
            .subresourceRange = {.aspectMask     = vk::ImageAspectFlagBits::eColor,
                                 .baseMipLevel   = 0,
                                 .levelCount     = 1,
                                 .baseArrayLayer = 0,
                                 .layerCount     = 1},
        };
        auto ViewRes = Context.Device.createImageView(ViewCI);
        if (ViewRes.result != vk::Result::eSuccess) {
            vmaDestroyImage(Context.Allocator, RawImage, RawAlloc);
            return std::unexpected(ErrorMessage("VulkanSampledTexture::Create: vkCreateImageView failed"));
        }

        auto Texture =
            std::make_shared<VulkanDeviceTexture>(Context.Allocator, VkImage, RawAlloc, std::move(ViewRes.value));
        auto Completion = VulkanImmediateContext::CompletionDesc{
            .ConsumerQueue = VulkanImmediateQueue::Graphics,
            .OnComplete =
                [Staging, Texture, OnReady = std::move(OnReady)]() mutable {
                    if (OnReady)
                        OnReady();
                },
        };
        if (auto R =
                Texture->CopyFrom(*Staging, Context.Immediate, Desc.Width, Desc.Height, VkFmt, std::move(Completion));
            !R)
            return std::unexpected(R.error().Append("VulkanSampledTexture::Create: transfer submission failed"));
        return std::make_unique<VulkanSampledTexture>(std::move(Texture), Desc.Width, Desc.Height);
    }

    // ── RHISampledTexture interface ──────────────────────────────────

    [[nodiscard]] auto GetWidth() const -> Uint32 override {
        return m_Width;
    }
    [[nodiscard]] auto GetHeight() const -> Uint32 override {
        return m_Height;
    }

    // ── Vulkan-specific accessors ──────────────────────────────────────

    [[nodiscard]] auto GetVkImage() const -> vk::Image {
        return m_Texture->GetImage();
    }
    [[nodiscard]] auto GetVkImageView() const -> vk::ImageView {
        return m_Texture->GetImageView();
    }

  private:
    SPtr<VulkanDeviceTexture> m_Texture = nullptr;
    Uint32                    m_Width   = 0;
    Uint32                    m_Height  = 0;
};

class VulkanRenderTarget final : public RHIRenderTarget {
  public:
    VulkanRenderTarget(
        SPtr<VulkanDeviceTexture> Tex, Uint32 Width, Uint32 Height, RHIFormat Format, RHITextureUsage Usage)
        : m_Texture(std::move(Tex)), m_Width(Width), m_Height(Height), m_Format(Format), m_Usage(Usage) {}

    ~VulkanRenderTarget() override = default;

    VulkanRenderTarget(const VulkanRenderTarget&)                    = delete;
    auto operator=(const VulkanRenderTarget&) -> VulkanRenderTarget& = delete;
    VulkanRenderTarget(VulkanRenderTarget&&)                         = delete;
    auto operator=(VulkanRenderTarget&&) -> VulkanRenderTarget&      = delete;

    [[nodiscard]] static auto Create(const VulkanResourceContext& Context, const RHIRenderTargetDesc& Desc)
        -> std::expected<UPtr<VulkanRenderTarget>, ErrorMessage> {
        if (Desc.Width == 0 || Desc.Height == 0)
            return std::unexpected(ErrorMessage("VulkanRenderTarget::Create: invalid desc (zero dimensions)"));
        if (Desc.Format == RHIFormat::Unknown)
            return std::unexpected(ErrorMessage("VulkanRenderTarget::Create: invalid desc (unknown format)"));

        const bool IsDepth =
            (static_cast<Uint32>(Desc.Usage) & static_cast<Uint32>(RHITextureUsage::DepthStencil)) != 0;
        const bool IsColor =
            (static_cast<Uint32>(Desc.Usage) & static_cast<Uint32>(RHITextureUsage::RenderTarget)) != 0;
        const bool IsFrameOutput =
            (static_cast<Uint32>(Desc.Usage) & static_cast<Uint32>(RHITextureUsage::FrameOutput)) != 0;
        const bool IsStorage =
            (static_cast<Uint32>(Desc.Usage) & static_cast<Uint32>(RHITextureUsage::ShaderStorage)) != 0;
        const bool IsShaderResource =
            (static_cast<Uint32>(Desc.Usage) & static_cast<Uint32>(RHITextureUsage::ShaderResource)) != 0;
        if (IsStorage && IsDepth)
            return std::unexpected(
                ErrorMessage("VulkanRenderTarget::Create: storage usage is not supported for depth targets"));
        if (!IsDepth && !IsColor)
            return std::unexpected(ErrorMessage("VulkanRenderTarget::Create: missing attachment usage"));

        auto Usage = vk::ImageUsageFlags{};
        if (IsDepth)
            Usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        if (IsColor)
            Usage |= vk::ImageUsageFlagBits::eColorAttachment;
        if (IsFrameOutput)
            Usage |= vk::ImageUsageFlagBits::eTransferSrc;
        if (IsStorage)
            Usage |= vk::ImageUsageFlagBits::eStorage;
        if (IsShaderResource)
            Usage |= vk::ImageUsageFlagBits::eSampled;

        const auto          VkFmt              = ToVkFormat(Desc.Format);
        const bool          bConcurrentSharing = Context.GraphicsFamily != Context.TransferFamily;
        const std::array    QueueFamilies{Context.GraphicsFamily, Context.TransferFamily};
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

        VmaAllocationCreateInfo ImageAllocInfo{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};

        VkImage           RawImage = nullptr;
        VmaAllocation     RawAlloc = nullptr;
        VkImageCreateInfo RawCI    = static_cast<VkImageCreateInfo>(ImageCI);
        if (vmaCreateImage(Context.Allocator, &RawCI, &ImageAllocInfo, &RawImage, &RawAlloc, nullptr) != VK_SUCCESS)
            return std::unexpected(ErrorMessage("VulkanRenderTarget::Create: vmaCreateImage failed"));

        auto       VkImage = static_cast<vk::Image>(RawImage);
        const auto Aspect  = IsDepth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor;

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
        auto ViewRes = Context.Device.createImageView(ViewCI);
        if (ViewRes.result != vk::Result::eSuccess) {
            vmaDestroyImage(Context.Allocator, RawImage, RawAlloc);
            return std::unexpected(ErrorMessage("VulkanRenderTarget::Create: vkCreateImageView failed"));
        }

        auto Tex =
            std::make_shared<VulkanDeviceTexture>(Context.Allocator, VkImage, RawAlloc, std::move(ViewRes.value));
        return std::make_unique<VulkanRenderTarget>(std::move(Tex), Desc.Width, Desc.Height, Desc.Format, Desc.Usage);
    }

    [[nodiscard]] auto GetWidth() const -> Uint32 override {
        return m_Width;
    }
    [[nodiscard]] auto GetHeight() const -> Uint32 override {
        return m_Height;
    }
    [[nodiscard]] auto GetFormat() const -> RHIFormat override {
        return m_Format;
    }
    [[nodiscard]] auto GetUsage() const -> RHITextureUsage override {
        return m_Usage;
    }

    [[nodiscard]] auto GetVkImage() const -> vk::Image {
        return m_Texture->GetImage();
    }
    [[nodiscard]] auto GetVkImageView() const -> vk::ImageView {
        return m_Texture->GetImageView();
    }

  private:
    SPtr<VulkanDeviceTexture> m_Texture = nullptr;
    Uint32                    m_Width   = 0;
    Uint32                    m_Height  = 0;
    RHIFormat                 m_Format  = RHIFormat::Unknown;
    RHITextureUsage           m_Usage   = RHITextureUsage::None;
};

} // namespace SoulEngine
