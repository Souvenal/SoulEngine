module;

#include <vk_mem_alloc.h>

// magic_enum::enum_flags_test is not exported by the v0.9.8 module.
#include <magic_enum/magic_enum_flags.hpp>

export module RHIVulkan.Texture;

import Core;
import TaskGraph;
import vulkan;
import RHI;
import std;

import RHIVulkan.Types;
import RHIVulkan.Context;
import RHIVulkan.Debug;
import RHIVulkan.Staging;

namespace SoulEngine {

/// @brief Unified backend texture: one identity covering sampled and render-target roles.
///
/// Usage bits come from the creation Desc union. Owns VkImage + whole-chain VkImageView + per-mip
/// VkImageViews + the VMA allocation lifecycle directly (no intermediate device-texture wrapper). Creation is
/// synchronous and byte-free (same contract as persistent buffers); texel data flows separately through
/// UploadTexture, staged into the staging belt's frame-start image lane (→ TransferDst → ShaderReadOnly
/// through the image tracker) — this class performs no upload.
export class VulkanTexture final : public RHITexture {
  public:
    /// @brief Forward the identity to the RHI base; prefer Create().
    ///
    /// Direct construction is public for make_unique. Create owns validation and fills the native handles;
    /// the constructor only forwards the identity to the RHI base.
    VulkanTexture(String Name, const RHITextureDesc& Desc);

    /// @brief Destruction runs on the RHI thread (deferred-deletion drain).
    ///
    /// The same thread that records with the tracker — safe to Forget here. GPU retirement rides the backend
    /// deferred deletion queue (the same virtual timeline event VMA buffers use): the frame-start upload
    /// batch's image lane snapshots raw image handles without retaining the texture, so final destruction
    /// must wait for the GPU.
    ~VulkanTexture() override;

    VulkanTexture(const VulkanTexture&)                    = delete;
    auto operator=(const VulkanTexture&) -> VulkanTexture& = delete;
    VulkanTexture(VulkanTexture&&)                         = delete;
    auto operator=(VulkanTexture&&) -> VulkanTexture&      = delete;

    [[nodiscard]] static auto Create(const VulkanResourceContext& Context, StringView Name, const RHITextureDesc& Desc)
        -> std::expected<UPtr<VulkanTexture>, ErrorMessage>;

    [[nodiscard]] auto GetVkImage() const -> vk::Image;
    [[nodiscard]] auto GetVkImageView() const -> vk::ImageView;
    /// Per-mip level-1 view. Precondition: Mip < GetMipLevelCount().
    [[nodiscard]] auto GetVkImageView(Uint32 Mip) const -> vk::ImageView;

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
export class VulkanTextureManager final {
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
        -> std::expected<VulkanTextureManager, ErrorMessage>;

    /// @brief Release the borrowed references.
    auto Shutdown() -> void;

    /// @brief Synchronous creation on the calling thread (VMA locks internally;
    /// vkCreateImage/vkCreateImageView are device-level). The ref publishes Ready at return.
    /// @param Name  logical RHI name; becomes the debug-utils object name.
    /// @param Desc  texture description; only GpuOnly placement is supported.
    [[nodiscard]] auto CreateTexture(StringView Name, const RHITextureDesc& Desc)
        -> std::expected<RHIRef<RHITexture>, ErrorMessage>;

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
        -> std::expected<void, ErrorMessage>;

  private:
    const VulkanResourceContext* m_Context = nullptr; ///< borrowed resource context
    VulkanStagingBelt*           m_Staging = nullptr; ///< borrowed staging belt, owned by the render device
};

} // namespace SoulEngine
