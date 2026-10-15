module;

#include <vk_mem_alloc.h>

// magic_enum::enum_flags_test is not exported by the v0.9.8 module.
#include <magic_enum/magic_enum_flags.hpp>

export module Vulkan:Texture;

import Core;
import TaskGraph;
import vulkan;
import RHI;
import std;

import :Types;
import :Context;
import :Debug;
import :Staging;

namespace SoulEngine {

/// Unified backend texture: one identity covering the old sampled texture and render target. Usage bits come
/// from the creation Desc union. Owns VkImage + whole-chain VkImageView + per-mip VkImageViews + the VMA
/// allocation lifecycle directly (no intermediate device-texture wrapper). Creation is synchronous and
/// byte-free (same contract as persistent buffers); texel data flows separately through UploadTexture, staged
/// into the staging belt's frame-start image lane (→ TransferDst → ShaderReadOnly through the image tracker)
/// — this class performs no upload.
class VulkanTexture final : public RHITexture {
  public:
    /// Direct construction is public for make_unique; prefer Create(). Create owns validation and fills the
    /// native handles; the constructor only forwards the identity to the RHI base.
    VulkanTexture(String Name, const RHITextureDesc& Desc) : RHITexture(std::move(Name), Desc) {}

    /// Destruction runs on the RHI thread (deferred-deletion drain), the same thread that records with the
    /// tracker — safe to Forget here. GPU retirement rides the backend deferred deletion queue (the same
    /// virtual timeline event VMA buffers use): the frame-start upload batch's image lane snapshots raw image
    /// handles without retaining the texture, so final destruction must wait for the GPU.
    ~VulkanTexture() override {
        if (m_ImageTracker && m_Image)
            m_ImageTracker->Forget(m_Image);
        if (m_Allocation)
            m_DeferredQueue->Enqueue(m_Image, m_Allocation);
    }

    VulkanTexture(const VulkanTexture&)                    = delete;
    auto operator=(const VulkanTexture&) -> VulkanTexture& = delete;
    VulkanTexture(VulkanTexture&&)                         = delete;
    auto operator=(VulkanTexture&&) -> VulkanTexture&      = delete;

    [[nodiscard]] static auto Create(const VulkanResourceContext& Context, StringView Name, const RHITextureDesc& Desc)
        -> std::expected<UPtr<VulkanTexture>, ErrorMessage> {
        if (Desc.Width == 0 || Desc.Height == 0)
            return std::unexpected(ErrorMessage("VulkanTexture::Create: invalid desc (zero dimensions)"));
        if (Desc.Format == RHIFormat::Unknown)
            return std::unexpected(ErrorMessage("VulkanTexture::Create: invalid desc (unknown format)"));

        const bool IsDepth   = magic_enum::enum_flags_test(Desc.Usage, RHITextureUsage::DepthStencil);
        const bool IsColor   = magic_enum::enum_flags_test(Desc.Usage, RHITextureUsage::RenderTarget);
        const bool IsStorage = magic_enum::enum_flags_test(Desc.Usage, RHITextureUsage::Storage);
        if (IsStorage && IsDepth)
            return std::unexpected(
                ErrorMessage("VulkanTexture::Create: storage usage is not supported for depth targets"));
        if (Desc.Usage == RHITextureUsage::None)
            return std::unexpected(ErrorMessage("VulkanTexture::Create: empty usage"));
        if (Desc.MemoryUsage == RHIMemoryUsage::Unknown)
            return std::unexpected(ErrorMessage("VulkanTexture::Create: memory usage is Unknown — a Desc is explicit"));
        if (Desc.MemoryUsage != RHIMemoryUsage::GpuOnly)
            return std::unexpected(ErrorMessage("VulkanTexture::Create: only GpuOnly textures are supported"));

        // step 1: validate mip count against the Vulkan full-chain limit floor(log2(max(w,h))) + 1 ==
        // bit_width(max(w,h)); partial chains are legal (D11).
        if (Desc.MipLevels == 0)
            return std::unexpected(ErrorMessage("VulkanTexture::Create: mipLevels must be at least 1"));
        const Uint32 MaxMipLevels = std::bit_width((std::max)(Desc.Width, Desc.Height));
        if (Desc.MipLevels > MaxMipLevels)
            return std::unexpected(ErrorMessage(Format("VulkanTexture::Create: mipLevels {} exceeds the "
                                                       "maximum {} for a {}x{} image",
                                                       Desc.MipLevels,
                                                       MaxMipLevels,
                                                       Desc.Width,
                                                       Desc.Height)));

        auto Usage = vk::ImageUsageFlags{};
        if (IsDepth)
            Usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        if (IsColor)
            Usage |= vk::ImageUsageFlagBits::eColorAttachment;
        if (magic_enum::enum_flags_test(Desc.Usage, RHITextureUsage::TransferSrc))
            Usage |= vk::ImageUsageFlagBits::eTransferSrc;
        if (magic_enum::enum_flags_test(Desc.Usage, RHITextureUsage::TransferDst))
            Usage |= vk::ImageUsageFlagBits::eTransferDst;
        if (IsStorage)
            Usage |= vk::ImageUsageFlagBits::eStorage;
        if (magic_enum::enum_flags_test(Desc.Usage, RHITextureUsage::Sampled))
            Usage |= vk::ImageUsageFlagBits::eSampled;
        // GpuOnly placement derives transfer-dst at creation (same contract as GpuOnly buffers) so
        // UploadTexture works without a declared usage bit.
        Usage |= vk::ImageUsageFlagBits::eTransferDst;

        const auto VkFmt = ToVkFormat(Desc.Format);
        if (VkFmt == vk::Format::eUndefined)
            return std::unexpected(ErrorMessage("VulkanTexture::Create: unsupported texture format"));
        const vk::ImageAspectFlags Aspect = IsDepth ? vk::ImageAspectFlags{vk::ImageAspectFlagBits::eDepth}
                                                    : vk::ImageAspectFlags{vk::ImageAspectFlagBits::eColor};

        const vk::ImageCreateInfo ImageCI{
            .imageType     = vk::ImageType::e2D,
            .format        = VkFmt,
            .extent        = {Desc.Width, Desc.Height, 1},
            .mipLevels     = Desc.MipLevels,
            .arrayLayers   = 1,
            .samples       = vk::SampleCountFlagBits::e1,
            .tiling        = vk::ImageTiling::eOptimal,
            .usage         = Usage,
            .sharingMode   = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        const VmaAllocationCreateInfo ImageAllocInfo{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};

        VkImage                 RawImage = nullptr;
        VmaAllocation           RawAlloc = nullptr;
        const VkImageCreateInfo RawCI    = static_cast<VkImageCreateInfo>(ImageCI);
        if (vmaCreateImage(Context.GetAllocator(), &RawCI, &ImageAllocInfo, &RawImage, &RawAlloc, nullptr) !=
            VK_SUCCESS)
            return std::unexpected(ErrorMessage("VulkanTexture::Create: vmaCreateImage failed"));

        const auto                    VkImage = static_cast<vk::Image>(RawImage);
        // step 2: whole-chain default view; sampled/storage descriptors use it unless a per-mip view is
        // requested. RemainingMipLevels keeps 1-mip images byte-identical to the old hardcoded levelCount = 1
        // behavior (D2).
        const vk::ImageViewCreateInfo ViewCI{
            .image            = VkImage,
            .viewType         = vk::ImageViewType::e2D,
            .format           = VkFmt,
            .components       = {vk::ComponentSwizzle::eIdentity,
                                 vk::ComponentSwizzle::eIdentity,
                                 vk::ComponentSwizzle::eIdentity,
                                 vk::ComponentSwizzle::eIdentity},
            .subresourceRange = {.aspectMask     = Aspect,
                                 .baseMipLevel   = 0,
                                 .levelCount     = vk::RemainingMipLevels,
                                 .baseArrayLayer = 0,
                                 .layerCount     = 1},
        };
        auto ViewRes = Context.GetDevice().createImageView(ViewCI);
        if (ViewRes.result != vk::Result::eSuccess) {
            vmaDestroyImage(Context.GetAllocator(), RawImage, RawAlloc);
            return std::unexpected(ErrorMessage("VulkanTexture::Create: vkCreateImageView failed"));
        }

        // step 3: one level-1 view per mip, created eagerly so render code can bind any mip without a
        // view-creation round trip (D2: per-mip views are RT-owned).
        std::vector<vk::raii::ImageView> MipViews;
        MipViews.reserve(Desc.MipLevels);
        for (Uint32 Mip = 0; Mip < Desc.MipLevels; ++Mip) {
            const vk::ImageViewCreateInfo MipViewCI{
                .image      = VkImage,
                .viewType   = vk::ImageViewType::e2D,
                .format     = VkFmt,
                .components = {vk::ComponentSwizzle::eIdentity,
                               vk::ComponentSwizzle::eIdentity,
                               vk::ComponentSwizzle::eIdentity,
                               vk::ComponentSwizzle::eIdentity},
                .subresourceRange =
                    {.aspectMask = Aspect, .baseMipLevel = Mip, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
            };
            auto MipViewRes = Context.GetDevice().createImageView(MipViewCI);
            if (MipViewRes.result != vk::Result::eSuccess) {
                vmaDestroyImage(Context.GetAllocator(), RawImage, RawAlloc);
                return std::unexpected(
                    ErrorMessage(Format("VulkanTexture::Create: vkCreateImageView failed for mip {}", Mip)));
            }
            Context.GetDebugUtils().SetObjectName(*MipViewRes.value, Format("{}#ImageViewMip{}", Name, Mip));
            MipViews.push_back(std::move(MipViewRes.value));
        }

        // The tracker stores no image metadata: transition call sites resolve concrete ranges from their
        // payloads, and destruction Forget()s the subresource states on the RHI thread. Creation touches
        // nothing.
        auto Out             = std::make_unique<VulkanTexture>(String(Name), Desc);
        Out->m_Allocator     = Context.GetAllocator();
        Out->m_DeferredQueue = &Context.GetDeferredDeletionQueue();
        Out->m_ImageTracker  = &Context.GetImageTracker();
        Out->m_Image         = VkImage;
        Out->m_Allocation    = RawAlloc;
        Out->m_ImageView     = std::move(ViewRes.value);
        Out->m_MipViews      = std::move(MipViews);

        Context.GetDebugUtils().SetObjectName(Out->m_Image, Format("{}#Image", Out->GetName()));
        Context.GetDebugUtils().SetObjectName(Out->GetVkImageView(), Format("{}#ImageView", Out->GetName()));

        return Out;
    }

    [[nodiscard]] auto GetVkImage() const -> vk::Image {
        return m_Image;
    }
    [[nodiscard]] auto GetVkImageView() const -> vk::ImageView {
        return *m_ImageView;
    }
    /// Per-mip level-1 view. Precondition: Mip < GetMipLevelCount().
    [[nodiscard]] auto GetVkImageView(Uint32 Mip) const -> vk::ImageView {
        return *m_MipViews[Mip];
    }

  private:
    VmaAllocator                     m_Allocator     = nullptr; ///< VMA allocator that owns the image allocation
    vk::Image                        m_Image         = nullptr;
    VmaAllocation                    m_Allocation    = nullptr;
    vk::raii::ImageView              m_ImageView     = nullptr; ///< whole-chain default view
    // One level-1 view per mip; GetVkImageView(Mip) indexes this. The whole-chain default view above stays
    // the default binding for existing call sites (D2).
    std::vector<vk::raii::ImageView> m_MipViews      = {};
    // Non-owning: the tracker lives in VulkanResourceContext and outlives all textures (deferred deletion
    // drains before device shutdown).
    VulkanImageTracker*              m_ImageTracker  = nullptr;
    // Non-owning: the queue lives in VulkanResourceContext and outlives all textures.
    VulkanDeferredDeletionQueue*     m_DeferredQueue = nullptr;
};

// VulkanTextureManager — creation dispatch and upload front-end

/// @brief The single home of texture concerns: creation dispatch and the upload front-end.
///
/// CreateTexture allocates synchronously on the calling thread; Upload snapshots the bytes at the call site
/// and hands a frame-affined task to the RHI thread, which stages the texels through the device-owned staging
/// belt's image lane. The render device owns one instance and forwards; the upload task is RHI-thread
/// confined.
class VulkanTextureManager final {
  public:
    VulkanTextureManager()                                               = default;
    VulkanTextureManager(const VulkanTextureManager&)                    = delete;
    auto operator=(const VulkanTextureManager&) -> VulkanTextureManager& = delete;
    VulkanTextureManager(VulkanTextureManager&&)                         = default;
    auto operator=(VulkanTextureManager&&) -> VulkanTextureManager&      = default;

    /// @brief Create the manager, borrowing the device-owned staging belt.
    /// @param Context  resource context providing the VMA allocator, device, and debug utils.
    /// @param Staging  device-owned staging belt, borrowed for the frame-start image lane.
    [[nodiscard]] static auto Create(const VulkanResourceContext& Context, VulkanStagingBelt& Staging)
        -> std::expected<VulkanTextureManager, ErrorMessage> {
        VulkanTextureManager Manager;
        Manager.m_Context = &Context;
        Manager.m_Staging = &Staging;
        return Manager;
    }

    /// @brief Release the borrowed references.
    auto Shutdown() -> void {
        m_Staging = nullptr;
        m_Context = nullptr;
    }

    /// @brief Synchronous creation on the calling thread (VMA locks internally;
    /// vkCreateImage/vkCreateImageView are device-level). The ref publishes Ready at return.
    /// @param Name  logical RHI name; becomes the debug-utils object name.
    /// @param Desc  texture description; only GpuOnly placement is supported.
    [[nodiscard]] auto CreateTexture(StringView Name, const RHITextureDesc& Desc)
        -> std::expected<RHIRef<RHITexture>, ErrorMessage> {
        auto Result = VulkanTexture::Create(*m_Context, Name, Desc);
        if (!Result)
            return std::unexpected(Result.error());
        auto Resource = RHIRef<RHITexture>::Create();
        if (auto Publish = Resource.Publish(std::move(*Result), RHIRefState::Ready); !Publish)
            return std::unexpected(Publish.error());
        return Resource;
    }

    /// @brief Upload a texture's full mip-0 texels: snapshot the bytes, then hand a
    /// frame-affined task to the RHI thread — the same contract as UploadBuffer.
    ///
    /// The task (drained at the matching RHI frame, before Execute) stages the bytes into the belt's image
    /// lane; Execute records the copy ahead of the frame's passes, so same-frame consumers are safe by
    /// submission order. Staging failure is fail-soft per entry (log + skip, buffer precedent): the caller
    /// may simply re-upload.
    /// @param Texture  destination texture ref.
    /// @param Data     full mip-0 texel snapshot; its size must equal Width × Height × Channels.
    [[nodiscard]] auto Upload(RHIRef<RHITexture> Texture, std::span<const std::byte> Data)
        -> std::expected<void, ErrorMessage> {
        if (Data.empty())
            return std::unexpected(ErrorMessage("UploadTexture: data is empty"));
        // Creation is synchronous, so the payload is always Ready here and the upload contract validates
        // fail-fast at the call site.
        const auto* Payload = Texture.TryGet();
        if (!Payload)
            return std::unexpected(ErrorMessage("UploadTexture: texture ref is not Ready"));
        if (magic_enum::enum_flags_test(Payload->GetUsage(), RHITextureUsage::DepthStencil))
            return std::unexpected(ErrorMessage("UploadTexture: depth textures are not upload targets"));
        const Uint64 ExpectedSize =
            static_cast<Uint64>(Payload->GetWidth()) * Payload->GetHeight() * Payload->GetChannels();
        if (Data.size_bytes() != ExpectedSize)
            return std::unexpected(ErrorMessage(
                Format("UploadTexture: data size {} does not match mip-0 size {}", Data.size_bytes(), ExpectedSize)));

        auto EnqueueResult = TaskGraph::Get().EnqueueFrameTask(
            ThreadQueue::RHI,
            [this,
             Texture  = std::move(Texture),
             Snapshot = std::vector<std::byte>{Data.begin(), Data.end()}]() mutable {
                auto* TaskPayload = Texture.TryGet();
                if (!TaskPayload) {
                    LogWarning("UploadTexture: texture ref is not Ready when the upload task ran");
                    return;
                }
                auto& VulkanTex = static_cast<VulkanTexture&>(*TaskPayload);
                m_Staging->EnqueueImageUpload(
                    VulkanTex.GetName(), VulkanTex.GetVkImage(), VulkanTex.GetWidth(), VulkanTex.GetHeight(), Snapshot);
            });
        if (!EnqueueResult)
            return std::unexpected(EnqueueResult.error().Append("UploadTexture: frame task enqueue failed"));
        return {};
    }

  private:
    const VulkanResourceContext* m_Context = nullptr; ///< borrowed resource context
    VulkanStagingBelt*           m_Staging = nullptr; ///< borrowed staging belt, owned by the render device
};

} // namespace SoulEngine
