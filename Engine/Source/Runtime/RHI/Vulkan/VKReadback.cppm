module;

#include <vk_mem_alloc.h>

export module Vulkan:Readback;

import Core;
import vulkan;
import RHI;
import std;

import :Context;
import :Debug;
import :Semaphore;

namespace SoulEngine {

/// @brief Backend realization of RHIReadback: a persistent mapped ring written by GPU transfer work.
///
/// Flow: VulkanRenderDevice::CreateReadback (ring depth = frames in flight + 1) → the readback command
/// allocates one slot per frame → the game thread polls TryRead(). Destruction rides the deferred deletion
/// queue. The device (resource context) must outlive the object.
export class VulkanReadback final : public RHIReadback {
  public:
    VulkanReadback(String Name, Uint64 SizeBytes, VulkanTimelineSemaphore& CompletedTimeline, Uint32 RingSize);

    /// @brief Destruction rides the backend deferred deletion queue.
    ///
    /// A virtual timeline event keeps the handles valid until the next real submission completes (the last
    /// GPU use cannot be later than the destruction frame's own submission).
    ~VulkanReadback() override;
    VulkanReadback(const VulkanReadback&)                    = delete;
    auto operator=(const VulkanReadback&) -> VulkanReadback& = delete;
    VulkanReadback(VulkanReadback&&)                         = delete;
    auto operator=(VulkanReadback&&) -> VulkanReadback&      = delete;

    /// @brief Synchronous creation; the caller publishes Ready at return.
    ///
    /// VMA locks internally, vkCreateBuffer is device-level thread-safe.
    [[nodiscard]] static auto
    Create(const VulkanResourceContext& Context, StringView Name, Uint64 SizeBytes, Uint32 RingSize)
        -> std::expected<UPtr<VulkanReadback>, ErrorMessage>;

    [[nodiscard]] auto GetVkBuffer() const -> vk::Buffer;
    /// @brief Pick the oldest-stamped slot and retag it with a fresh host timeline value.
    ///
    /// Ring depth = frames in flight + 1 keeps the victim one generation older than any slot a reader may
    /// hold: the frame-slot wait at BeginFrame bounds in-flight writes below the ring depth, and stamps
    /// increase monotonically with allocation order.
    [[nodiscard]] auto AllocateWriteOffset() -> Uint64;
    [[nodiscard]] auto GetSlotOffset(Uint32 Slot) const -> Uint64;

    /// @brief Read the newest completed slot's bytes, if any.
    ///
    /// Readers consume only slots whose stamp the device timeline has passed (newest completed wins). The
    /// completed-value query borrows the resource-context graphics timeline, so the read is valid only while
    /// the resource context is alive.
    [[nodiscard]] auto TryRead() const -> std::vector<std::byte> override;

  private:
    VmaAllocator                     m_Allocator         = nullptr;
    VulkanDeferredDeletionQueue*     m_DeferredQueue     = nullptr; // borrowed; owned by the resource context
    vk::Buffer                       m_Buffer            = nullptr;
    VmaAllocation                    m_Allocation        = nullptr;
    std::byte*                       m_Mapped            = nullptr;
    VulkanTimelineSemaphore*         m_CompletedTimeline = nullptr; // borrowed; device outlives this readback
    Uint32                           m_RingSize          = 0;
    std::vector<std::atomic<Uint64>> m_WriteValues       = {};
    bool                             m_Coherent          = false;
};

} // namespace SoulEngine
