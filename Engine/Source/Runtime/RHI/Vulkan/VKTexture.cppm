module;

#include <vk_mem_alloc.h>

export module Vulkan:Texture;

import Core;
import vulkan;
import RHI;
import std;

import :Types;
import :Buffer;
import :ImmediateContext;
import :TransferCompletionQueue;
import :Descriptor;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

// ═════════════════════════════════════════════════════════════════════════════
// Vulkan::SampledTexture — GPU sampled texture resource
// ═════════════════════════════════════════════════════════════════════════════

/// Concrete Vulkan sampled texture. Owns VkImage + VkImageView + VMA allocation.
/// Created via static factory Create(). Move-only, non-copyable.
class SampledTexture final : public RHI::SampledTexture {
  public:
    SampledTexture() = default;

    SampledTexture(vk::Image             Image,
                   vk::raii::ImageView&& ImageView,
                   VmaAllocation         Allocation,
                   VmaAllocator          Allocator,
                   vk::raii::Device*     Device,
                   Uint32                Width,
                   Uint32                Height,
                   Uint32                DescriptorSlot)
        : m_Image(Image),
          m_ImageView(std::move(ImageView)),
          m_Allocation(Allocation),
          m_Allocator(Allocator),
          m_Device(Device),
          m_Width(Width),
          m_Height(Height),
          m_DescriptorSlot(DescriptorSlot) {}

    ~SampledTexture() {
        Destroy();
    }

    SampledTexture(SampledTexture&& Other) noexcept
        : m_Image(std::exchange(Other.m_Image, nullptr)),
          m_ImageView(std::move(Other.m_ImageView)),
          m_Allocation(std::exchange(Other.m_Allocation, nullptr)),
          m_Allocator(std::exchange(Other.m_Allocator, nullptr)),
          m_Device(std::exchange(Other.m_Device, nullptr)),
          m_Width(std::exchange(Other.m_Width, 0u)),
          m_Height(std::exchange(Other.m_Height, 0u)),
          m_DescriptorSlot(std::exchange(Other.m_DescriptorSlot, 0u)) {}

    auto operator=(SampledTexture&& Other) noexcept -> SampledTexture& {
        if (this != &Other) {
            std::swap(m_Image, Other.m_Image);
            std::swap(m_ImageView, Other.m_ImageView);
            std::swap(m_Allocation, Other.m_Allocation);
            std::swap(m_Allocator, Other.m_Allocator);
            std::swap(m_Device, Other.m_Device);
            std::swap(m_Width, Other.m_Width);
            std::swap(m_Height, Other.m_Height);
            std::swap(m_DescriptorSlot, Other.m_DescriptorSlot);
        }
        return *this;
    }

    SampledTexture(const SampledTexture&)                    = delete;
    auto operator=(const SampledTexture&) -> SampledTexture& = delete;

    /// Static factory: upload pixel data to GPU texture via staging buffer.
    /// Allocates a bindless descriptor slot and writes the ImageView.
    [[nodiscard]] static auto Create(const RHI::SampledTextureDesc& Desc,
                                     VmaAllocator                   Alloc,
                                     vk::raii::Device&              Dev,
                                     ImmediateContext&              ImmCtx,
                                     TransferCompletionQueue&       CompletionQueue,
                                     DescriptorManager&             DescMgr)
        -> std::expected<RHI::SampledTextureCreateResult, ErrorMessage> {
        if (!Desc.Data || Desc.Width == 0 || Desc.Height == 0)
            return std::unexpected(ErrorMessage("SampledTexture::Create: invalid desc (null data or zero dimensions)"));

        Uint64     PixelSize = static_cast<Uint64>(Desc.Width) * Desc.Height * Desc.Channels;
        vk::Format VkFmt     = SoulEngine::RHI::Vulkan::ToVkFormat(static_cast<RHI::Format>(Desc.Format));

        // ── Staging buffer ─────────────────────────────────────────────
        auto StagingRes = HostBuffer::Create(PixelSize, vk::BufferUsageFlagBits::eTransferSrc, *Dev, Alloc);
        if (!StagingRes)
            return std::unexpected(StagingRes.error().Append("SampledTexture::Create: staging creation failed"));
        auto Staging = std::move(*StagingRes);

        if (auto R = Staging.Upload(Desc.Data, PixelSize); !R)
            return std::unexpected(R.error().Append("SampledTexture::Create: staging upload failed"));

        // ── Create VkImage ─────────────────────────────────────────────
        vk::ImageCreateInfo ImageCI{
            .imageType     = vk::ImageType::e2D,
            .format        = VkFmt,
            .extent        = {Desc.Width, Desc.Height, 1},
            .mipLevels     = 1,
            .arrayLayers   = 1,
            .samples       = vk::SampleCountFlagBits::e1,
            .tiling        = vk::ImageTiling::eOptimal,
            .usage         = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
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
            return std::unexpected(ErrorMessage("SampledTexture::Create: vmaCreateImage failed"));

        auto VkImage = static_cast<vk::Image>(RawImage);

        // ── Transition: Undefined -> TransferDstOptimal ─────────────────
        auto CopyResult = ImmCtx.SubmitTransfer(
            [&](const vk::raii::CommandBuffer& CmdBuf) {
                // Barrier: Undefined -> TransferDst
                vk::ImageMemoryBarrier2 Barrier1{
                    .srcStageMask        = vk::PipelineStageFlagBits2::eNone,
                    .srcAccessMask       = vk::AccessFlagBits2::eNone,
                    .dstStageMask        = vk::PipelineStageFlagBits2::eTransfer,
                    .dstAccessMask       = vk::AccessFlagBits2::eTransferWrite,
                    .oldLayout           = vk::ImageLayout::eUndefined,
                    .newLayout           = vk::ImageLayout::eTransferDstOptimal,
                    .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                    .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                    .image               = VkImage,
                    .subresourceRange    = {.aspectMask     = vk::ImageAspectFlagBits::eColor,
                                            .baseMipLevel   = 0,
                                            .levelCount     = 1,
                                            .baseArrayLayer = 0,
                                            .layerCount     = 1},
                };
                CmdBuf.pipelineBarrier2(vk::DependencyInfo{
                    .imageMemoryBarrierCount = 1,
                    .pImageMemoryBarriers    = &Barrier1,
                });

                // Copy buffer -> image
                vk::BufferImageCopy Region{
                    .bufferOffset      = 0,
                    .bufferRowLength   = 0, // tightly packed
                    .bufferImageHeight = 0,
                    .imageSubresource  = {.aspectMask     = vk::ImageAspectFlagBits::eColor,
                                          .mipLevel       = 0,
                                          .baseArrayLayer = 0,
                                          .layerCount     = 1},
                    .imageOffset       = {0, 0, 0},
                    .imageExtent       = {Desc.Width, Desc.Height, 1},
                };
                CmdBuf.copyBufferToImage(Staging.Get(), VkImage, vk::ImageLayout::eTransferDstOptimal, {Region});

                // Barrier: TransferDst -> ShaderReadOnly
                vk::ImageMemoryBarrier2 Barrier2{
                    .srcStageMask        = vk::PipelineStageFlagBits2::eTransfer,
                    .srcAccessMask       = vk::AccessFlagBits2::eTransferWrite,
                    .dstStageMask        = vk::PipelineStageFlagBits2::eFragmentShader,
                    .dstAccessMask       = vk::AccessFlagBits2::eShaderSampledRead,
                    .oldLayout           = vk::ImageLayout::eTransferDstOptimal,
                    .newLayout           = vk::ImageLayout::eShaderReadOnlyOptimal,
                    .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                    .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                    .image               = VkImage,
                    .subresourceRange    = {.aspectMask     = vk::ImageAspectFlagBits::eColor,
                                            .baseMipLevel   = 0,
                                            .levelCount     = 1,
                                            .baseArrayLayer = 0,
                                            .layerCount     = 1},
                };
                CmdBuf.pipelineBarrier2(vk::DependencyInfo{
                    .imageMemoryBarrierCount = 1,
                    .pImageMemoryBarriers    = &Barrier2,
                });
            });

        if (!CopyResult)
            return std::unexpected(CopyResult.error().Append("SampledTexture::Create: transfer submission failed"));

        // ── Create ImageView ───────────────────────────────────────────
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
        auto ViewRes = Dev.createImageView(ViewCI);
        if (ViewRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("SampledTexture::Create: vkCreateImageView failed"));
        auto VkImageView = std::move(ViewRes.value);

        // ── Bindless descriptor ────────────────────────────────────────
        Uint32 Slot = DescMgr.AllocateTexture();
        DescMgr.WriteTextureSlot(Slot, *VkImageView, vk::ImageLayout::eShaderReadOnlyOptimal);

        // ── Defer staging destruction ──────────────────────────────────
        Staging.DeferredDelete(CompletionQueue, *CopyResult);

        return RHI::SampledTextureCreateResult{
            .Texture = std::make_shared<SampledTexture>(
                VkImage, std::move(VkImageView), RawAlloc, Alloc, &Dev, Desc.Width, Desc.Height, Slot),
            .UploadCompletion = *CopyResult,
        };
    }

    // ── RHI::SampledTexture interface ──────────────────────────────────

    [[nodiscard]] auto GetWidth() const -> Uint32 override {
        return m_Width;
    }
    [[nodiscard]] auto GetHeight() const -> Uint32 override {
        return m_Height;
    }

    // ── Vulkan-specific accessors ──────────────────────────────────────

    [[nodiscard]] auto GetVkImage() const -> vk::Image {
        return m_Image;
    }
    [[nodiscard]] auto GetVkImageView() const -> vk::ImageView {
        return *m_ImageView;
    }
    [[nodiscard]] auto GetDescriptorSlot() const -> Uint32 {
        return m_DescriptorSlot;
    }

  private:
    auto Destroy() -> void {
        // m_ImageView is vk::raii::ImageView — RAII destructor handles cleanup.
        if (m_Allocation)
            vmaDestroyImage(m_Allocator, static_cast<VkImage>(m_Image), m_Allocation);
    }

    vk::Image           m_Image          = nullptr;
    vk::raii::ImageView m_ImageView      = nullptr;
    VmaAllocation       m_Allocation     = nullptr;
    VmaAllocator        m_Allocator      = nullptr;
    vk::raii::Device*   m_Device         = nullptr;
    Uint32              m_Width          = 0;
    Uint32              m_Height         = 0;
    Uint32              m_DescriptorSlot = 0;
};

} // namespace SoulEngine::RHI::Vulkan
