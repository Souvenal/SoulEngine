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
import :DeletionQueue;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

// ═════════════════════════════════════════════════════════════════════════════
// DeviceTexture — reusable GPU image wrapper
// ═════════════════════════════════════════════════════════════════════════════

/// Owns VkImage + VkImageView + VMA allocation lifecycle.
///
/// For sampled textures: holds a bindless descriptor slot and releases it
/// on destruction.  Pass DescManager = nullptr to skip descriptor management
/// (e.g. for render targets owned by a different allocation path).
class DeviceTexture {
  public:
    DeviceTexture() = default;

    DeviceTexture(VmaAllocator          Alloc,
                  vk::Image             Image,
                  VmaAllocation         Allocation,
                  vk::raii::ImageView&& ImageView,
                  DescriptorManager*    DescManager,
                  Uint32                DescriptorSlot)
        : m_Allocator(Alloc),
          m_Image(Image),
          m_Allocation(Allocation),
          m_ImageView(std::move(ImageView)),
          m_DescManager(DescManager),
          m_DescriptorSlot(DescriptorSlot) {}

    ~DeviceTexture() {
        if (m_DescManager)
            m_DescManager->FreeTexture(m_DescriptorSlot);
        if (m_Allocation)
            vmaDestroyImage(m_Allocator, static_cast<VkImage>(m_Image), m_Allocation);
    }

    // Move-only.
    DeviceTexture(DeviceTexture&& Other) noexcept
        : m_Allocator(std::exchange(Other.m_Allocator, nullptr)),
          m_Image(std::exchange(Other.m_Image, nullptr)),
          m_Allocation(std::exchange(Other.m_Allocation, nullptr)),
          m_ImageView(std::move(Other.m_ImageView)),
          m_DescManager(std::exchange(Other.m_DescManager, nullptr)),
          m_DescriptorSlot(std::exchange(Other.m_DescriptorSlot, 0u)) {}

    auto operator=(DeviceTexture&& Other) noexcept -> DeviceTexture& {
        if (this != &Other) {
            std::swap(m_Allocator, Other.m_Allocator);
            std::swap(m_Image, Other.m_Image);
            std::swap(m_Allocation, Other.m_Allocation);
            std::swap(m_ImageView, Other.m_ImageView);
            std::swap(m_DescManager, Other.m_DescManager);
            std::swap(m_DescriptorSlot, Other.m_DescriptorSlot);
        }
        return *this;
    }

    DeviceTexture(const DeviceTexture&)  = delete;
    auto operator=(const DeviceTexture&) = delete;

    [[nodiscard]] auto GetImage() const -> vk::Image {
        return m_Image;
    }
    [[nodiscard]] auto GetImageView() const -> vk::ImageView {
        return *m_ImageView;
    }
    [[nodiscard]] auto GetDescriptorSlot() const -> Uint32 {
        return m_DescriptorSlot;
    }
    [[nodiscard]] auto GetAllocation() const -> VmaAllocation {
        return m_Allocation;
    }

    /// Submit a staging-buffer → image copy via ImmediateContext.
    /// Handles Undefined→TransferDst→ShaderReadOnly barriers.
    /// Returns upload completion token (caller defers staging destruction).
    [[nodiscard]] auto
    CopyFrom(HostBuffer& Staging, ImmediateContext& Ctx, Uint32 Width, Uint32 Height, vk::Format VkFmt)
        -> std::expected<GpuCompletionToken, ErrorMessage> {

        return Ctx.SubmitTransfer([&](const vk::raii::CommandBuffer& CmdBuf) {
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
            CmdBuf.pipelineBarrier2(vk::DependencyInfo{
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers    = &Barrier1,
            });

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
                .dstStageMask        = vk::PipelineStageFlagBits2::eFragmentShader,
                .dstAccessMask       = vk::AccessFlagBits2::eShaderSampledRead,
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
            CmdBuf.pipelineBarrier2(vk::DependencyInfo{
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers    = &Barrier2,
            });
        });
    }

  private:
    VmaAllocator        m_Allocator      = nullptr;
    vk::Image           m_Image          = nullptr;
    VmaAllocation       m_Allocation     = nullptr;
    vk::raii::ImageView m_ImageView      = nullptr;
    DescriptorManager*  m_DescManager    = nullptr;
    Uint32              m_DescriptorSlot = 0;
};

// ═════════════════════════════════════════════════════════════════════════════
// Vulkan::SampledTexture — GPU sampled texture resource
// ═════════════════════════════════════════════════════════════════════════════

class SampledTexture final : public RHI::SampledTexture {
  public:
    SampledTexture(SPtr<DeviceTexture> Tex, DeletionQueue& Queue, Uint32 Width, Uint32 Height)
        : m_Texture(std::move(Tex)), m_DeletionQueue(&Queue), m_Width(Width), m_Height(Height) {}

    ~SampledTexture() override {
        if (m_DeletionQueue)
            m_DeletionQueue->Enqueue(GetLastUsageToken(), [Tex = m_Texture]() {});
    }

    SampledTexture(const SampledTexture&)                    = delete;
    auto operator=(const SampledTexture&) -> SampledTexture& = delete;
    SampledTexture(SampledTexture&&)                         = delete;
    auto operator=(SampledTexture&&) -> SampledTexture&      = delete;

    /// Static factory: upload pixel data to GPU texture via staging buffer.
    [[nodiscard]] static auto Create(const RHI::SampledTextureDesc& Desc,
                                     VmaAllocator                   Alloc,
                                     vk::raii::Device&              Dev,
                                     ImmediateContext&              ImmCtx,
                                     TransferCompletionQueue&       CompletionQueue,
                                     DescriptorManager&             DescMgr,
                                     DeletionQueue&                 DelQueue)
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

        VmaAllocationCreateInfo ImageAllocInfo{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};

        VkImage           RawImage = nullptr;
        VmaAllocation     RawAlloc = nullptr;
        VkImageCreateInfo RawCI    = static_cast<VkImageCreateInfo>(ImageCI);
        if (vmaCreateImage(Alloc, &RawCI, &ImageAllocInfo, &RawImage, &RawAlloc, nullptr) != VK_SUCCESS)
            return std::unexpected(ErrorMessage("SampledTexture::Create: vmaCreateImage failed"));

        auto VkImage = static_cast<vk::Image>(RawImage);

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

        // ── Bindless descriptor ────────────────────────────────────────
        Uint32 Slot = DescMgr.AllocateTexture();
        DescMgr.WriteTextureSlot(Slot, *ViewRes.value, vk::ImageLayout::eShaderReadOnlyOptimal);

        // ── Copy staging → device via DeviceTexture ────────────────────
        auto Tex = std::make_shared<DeviceTexture>(Alloc, VkImage, RawAlloc, std::move(ViewRes.value), &DescMgr, Slot);

        auto CopyResult = Tex->CopyFrom(Staging, ImmCtx, Desc.Width, Desc.Height, VkFmt);
        if (!CopyResult)
            return std::unexpected(CopyResult.error().Append("SampledTexture::Create: transfer submission failed"));

        // ── Defer staging destruction ──────────────────────────────────
        Staging.DeferredDelete(CompletionQueue, *CopyResult);

        return RHI::SampledTextureCreateResult{
            .Texture          = std::make_shared<SampledTexture>(std::move(Tex), DelQueue, Desc.Width, Desc.Height),
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
        return m_Texture->GetImage();
    }
    [[nodiscard]] auto GetVkImageView() const -> vk::ImageView {
        return m_Texture->GetImageView();
    }
    [[nodiscard]] auto GetDescriptorSlot() const -> Uint32 {
        return m_Texture->GetDescriptorSlot();
    }

  private:
    SPtr<DeviceTexture> m_Texture       = nullptr;
    DeletionQueue*      m_DeletionQueue = nullptr;
    Uint32              m_Width         = 0;
    Uint32              m_Height        = 0;
};

} // namespace SoulEngine::RHI::Vulkan
