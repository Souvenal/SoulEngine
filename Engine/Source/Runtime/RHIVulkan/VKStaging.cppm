module;

export module RHIVulkan:Staging;

import Core;
import vulkan;
import RHI;
import std;

import :RingBuffer;
import :Types;
import :Context;
import :Debug;
import :Profiling;

namespace SoulEngine {

/// @brief One frame-start buffer upload batch entry.
///
/// The ring slice carries the already-copied bytes; the native destination handle and post-barrier access
/// mask are snapshotted from the payload at EnqueueBufferUpload time — a persistent buffer's handle is
/// immutable — so batch recording never has to resolve the payload again. No destination ref is kept: if the
/// producer's last ref dies before the copy is submitted, native destruction rides the backend deferred
/// deletion queue (a virtual timeline event), which keeps the handle valid until the submission completes;
/// the slice's ring range retires via the ring's timeline markers.
struct VulkanPendingBufferUpload {
    RingAllocation   Staging     = {};      ///< ring slice holding the copied bytes
    vk::Buffer       DstBuffer   = nullptr; ///< native destination handle, snapshotted at enqueue time
    /// @brief Accesses performed after the batch's visibility barrier.
    ///
    /// UE FRHITransitionInfo's AccessAfter / D3D12 StateAfter vocabulary: the dstAccessMask half, computed at
    /// enqueue time from the usage union.
    vk::AccessFlags2 AccessAfter = {};
};

/// @brief One frame-start texture upload batch entry.
///
/// Same ring-slice transport and no-retention contract as the buffer lane (the backend deferred deletion
/// queue covers VMA images too). The copy target is always mip 0 of a color image: UploadTexture performs
/// full-pixel mip-0 uploads only.
struct VulkanPendingImageUpload {
    RingAllocation Staging  = {};      ///< ring slice holding the copied texel bytes
    vk::Image      DstImage = nullptr; ///< native destination handle, snapshotted at enqueue time
    Uint32         Width    = 0;       ///< destination mip-0 extent
    Uint32         Height   = 0;
};

/// @brief The frame-start upload staging lane: owns the staging ring and the pending batch
/// entries of both resource kinds (buffer copies and mip-0 texture uploads).
///
/// Buffer upload frame tasks EnqueueBufferUpload() bytes into ring slices; texture upload frame tasks
/// EnqueueImageUpload() texel bytes the same way; Execute records the batch; Tick() runs at frame begin,
/// retiring the markers the device timeline has passed and sealing the previous frame's pending uploads
/// against the frame-begin timeline value. The render device owns the belt and the resource managers borrow
/// it. RHI-thread confined.
class VulkanStagingBelt {
  public:
    VulkanStagingBelt()                                                = default;
    VulkanStagingBelt(const VulkanStagingBelt&)                        = delete;
    auto operator=(const VulkanStagingBelt&) -> VulkanStagingBelt&     = delete;
    VulkanStagingBelt(VulkanStagingBelt&&) noexcept                    = default;
    auto operator=(VulkanStagingBelt&&) noexcept -> VulkanStagingBelt& = default;

    /// @brief Create the belt with its staging ring.
    /// @param Context          resource context providing the VMA allocator, debug utils, and the device
    ///                         timeline; the pointer is retained (borrowed) for batch self-annotation.
    /// @param InitialCapacity  initial ring capacity in bytes (the RHI.Vulkan.StagingRingBufferSize CVar).
    [[nodiscard]] static auto Create(const VulkanResourceContext& Context, Uint64 InitialCapacity)
        -> std::expected<VulkanStagingBelt, ErrorMessage>;

    /// @brief Upload frame-task path for GpuOnly payloads: bump-allocate a ring slice, host-copy
    /// the bytes, and queue the batch entry.
    ///
    /// DstBuffer is snapshotted from the destination payload — a persistent buffer's native handle is
    /// immutable — and Usage is folded into the post-barrier access mask here, so RecordUploadBatch never
    /// resolves the payload again. Fail-soft per entry (BLAS batch precedent): problems are logged and
    /// skipped.
    /// @param Name       destination buffer name, for logging.
    /// @param DstBuffer  native destination handle, snapshotted from the payload.
    /// @param Usage      destination usage union, drives the batch's visibility barrier mask.
    auto
    EnqueueBufferUpload(StringView Name, vk::Buffer DstBuffer, RHIBufferUsage Usage, std::span<const std::byte> Data)
        -> void;

    /// @brief Explicit-access entry: the same staging path with the post-barrier access mask
    /// supplied directly, for destinations outside the RHIBufferUsage vocabulary (e.g. a shader binding table
    /// read by trace rays).
    /// @param Name         destination buffer name, for logging.
    /// @param DstBuffer    native destination handle, snapshotted by the caller.
    /// @param AccessAfter  post-barrier access mask, folded into the batch's aggregated barrier.
    /// @param Data         byte snapshot to stage.
    auto EnqueueBufferUpload(StringView                 Name,
                             vk::Buffer                 DstBuffer,
                             vk::AccessFlags2           AccessAfter,
                             std::span<const std::byte> Data) -> void;

    /// @brief UploadTexture frame-task path: bump-allocate a ring slice, host-copy the texel bytes,
    /// and queue the image batch entry. RecordUploadBatch wraps one vkCmdCopyBufferToImage per entry in
    /// image-tracker transitions (→ TransferDst → ShaderReadOnly, mip 0, color aspect; the tracker supplies
    /// the current layout, so re-uploads transition correctly too).
    ///
    /// Fail-soft per entry (buffer lane precedent): problems are logged and skipped; UploadTexture is
    /// re-issuable, so a dropped entry is not permanent.
    /// @param Name      destination texture name, for logging.
    /// @param DstImage  native destination handle, snapshotted from the payload.
    /// @param Width     destination mip-0 width.
    /// @param Height    destination mip-0 height.
    /// @param Data      full mip-0 texel snapshot to stage.
    auto EnqueueImageUpload(
        StringView Name, vk::Image DstImage, Uint32 Width, Uint32 Height, std::span<const std::byte> Data) -> void;

    /// @brief Frame-start upload batch, two lanes. Buffers: record one vkCmdCopyBuffer2 per entry
    /// (slices are already filled by the upload frame tasks), then a single aggregated transfer-write →
    /// consumer-read barrier. Textures: wrap one vkCmdCopyBufferToImage per entry in image-tracker
    /// transitions (Undefined → TransferDst → ShaderReadOnly). A no-op when both lanes are empty.
    ///
    /// The buffer barrier lets later work in the same submission (the AS builds, recorded into their own
    /// secondary) and the frame's passes consume the bytes safely; the image lane's tracker transitions do
    /// the same for textures. Per-entry problems are logged and skipped (BLAS batch precedent). The ring
    /// ranges retire via the ring's timeline markers; destinations need no retention — their native
    /// destruction rides the backend deferred deletion queue.
    /// @param CmdBuffer  the upload batch's dedicated secondary command buffer.
    auto RecordUploadBatch(vk::raii::CommandBuffer& CmdBuffer) -> void;

  private:
    /// @brief Buffer lane of the frame-start batch.
    ///
    /// One copy per entry, then the aggregated visibility barrier. A no-op when the lane is empty.
    auto RecordBufferLane(vk::raii::CommandBuffer& CmdBuffer) -> void;

    /// @brief Image lane of the frame-start batch.
    ///
    /// One buffer→image copy per entry, wrapped in image-tracker transitions. The post-copy state is
    /// ShaderReadOnly — the same end state the old immediate-path upload produced; later prologue transitions
    /// take it from there. A no-op when the lane is empty.
    auto RecordImageLane(vk::raii::CommandBuffer& CmdBuffer) -> void;

  public:
    /// @brief Frame-begin relay: retire ring markers the device timeline has passed, then seal the
    /// previous frame's pending uploads against the frame-begin timeline value.
    /// @param FrameBeginTimelineValue  host-reserved frame-begin value, issued by the device Tick.
    auto Tick(Uint64 FrameBeginTimelineValue) -> void;

  private:
    const VulkanResourceContext*           m_Context              = nullptr; ///< borrowed; outlives the belt
    VulkanRingBuffer                       m_Ring                 = {};      ///< staging source ring
    std::vector<VulkanPendingBufferUpload> m_PendingBufferUploads = {};      ///< pending buffer batch entries
    std::vector<VulkanPendingImageUpload>  m_PendingImageUploads  = {};      ///< pending texture batch entries
};

} // namespace SoulEngine
