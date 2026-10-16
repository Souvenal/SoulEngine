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

export namespace SoulEngine {

/// @brief Backend realization of RHIReadback: a persistent mapped ring written by GPU transfer work.
///
/// Flow: VulkanRenderDevice::CreateReadback (ring depth = frames in flight + 1) → the readback command
/// allocates one slot per frame → the game thread polls TryRead(). Destruction rides the deferred deletion
/// queue. The device (resource context) must outlive the object.
class VulkanReadback final : public RHIReadback {
  public:
    VulkanReadback(String Name, Uint64 SizeBytes, VulkanTimelineSemaphore& CompletedTimeline, Uint32 RingSize)
        : RHIReadback(std::move(Name), SizeBytes),
          m_CompletedTimeline(&CompletedTimeline),
          m_RingSize(RingSize),
          m_WriteValues(RingSize) {
        for (auto& Value : m_WriteValues)
            Value.store(0, std::memory_order_relaxed);
    }

    /// @brief Destruction rides the backend deferred deletion queue.
    ///
    /// A virtual timeline event keeps the handles valid until the next real submission completes (the last
    /// GPU use cannot be later than the destruction frame's own submission).
    ~VulkanReadback() override {
        if (m_Allocation)
            m_DeferredQueue->Enqueue(m_Buffer, m_Allocation);
    }
    VulkanReadback(const VulkanReadback&)                    = delete;
    auto operator=(const VulkanReadback&) -> VulkanReadback& = delete;
    VulkanReadback(VulkanReadback&&)                         = delete;
    auto operator=(VulkanReadback&&) -> VulkanReadback&      = delete;

    /// @brief Synchronous creation; the caller publishes Ready at return.
    ///
    /// VMA locks internally, vkCreateBuffer is device-level thread-safe.
    [[nodiscard]] static auto
    Create(const VulkanResourceContext& Context, StringView Name, Uint64 SizeBytes, Uint32 RingSize)
        -> std::expected<UPtr<VulkanReadback>, ErrorMessage> {
        if (SizeBytes == 0 || RingSize == 0)
            return std::unexpected(ErrorMessage("VulkanReadback::Create: size and ring size must be non-zero"));

        auto Result = std::make_unique<VulkanReadback>(String(Name), SizeBytes, Context.GetTimeline(), RingSize);
        Result->m_Allocator     = Context.GetAllocator();
        Result->m_DeferredQueue = &Context.GetDeferredDeletionQueue();

        const Uint64         Size = SizeBytes * RingSize;
        vk::BufferCreateInfo BufCI{
            .size        = Size,
            .usage       = vk::BufferUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive,
        };

        VmaAllocationCreateInfo AllocInfo{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };

        VmaAllocationInfo AllocationInfo{};
        if (vmaCreateBuffer(Context.GetAllocator(),
                            reinterpret_cast<VkBufferCreateInfo*>(&BufCI),
                            &AllocInfo,
                            reinterpret_cast<VkBuffer*>(&Result->m_Buffer),
                            &Result->m_Allocation,
                            &AllocationInfo) != VK_SUCCESS)
            return std::unexpected(ErrorMessage("VulkanReadback::Create: VMA buffer creation failed"));
        if (!AllocationInfo.pMappedData) {
            vmaDestroyBuffer(Context.GetAllocator(), static_cast<VkBuffer>(Result->m_Buffer), Result->m_Allocation);
            Result->m_Buffer     = nullptr;
            Result->m_Allocation = nullptr;
            return std::unexpected(ErrorMessage("VulkanReadback::Create: allocation is not mapped"));
        }
        Result->m_Mapped = static_cast<std::byte*>(AllocationInfo.pMappedData);

        VkMemoryPropertyFlags MemoryProps = 0;
        vmaGetAllocationMemoryProperties(Context.GetAllocator(), Result->m_Allocation, &MemoryProps);
        Result->m_Coherent = (MemoryProps & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

        Context.GetDebugUtils().SetObjectName(Result->m_Buffer, Result->GetName());
        return Result;
    }

    [[nodiscard]] auto GetVkBuffer() const -> vk::Buffer {
        return m_Buffer;
    }
    /// @brief Pick the oldest-stamped slot and retag it with a fresh host timeline value.
    ///
    /// Ring depth = frames in flight + 1 keeps the victim one generation older than any slot a reader may
    /// hold: the frame-slot wait at BeginFrame bounds in-flight writes below the ring depth, and stamps
    /// increase monotonically with allocation order.
    [[nodiscard]] auto AllocateWriteOffset() -> Uint64 {
        Uint32 OldestSlot  = 0;
        Uint64 OldestValue = m_WriteValues[0].load(std::memory_order_acquire);
        for (Uint32 Slot = 1; Slot < m_RingSize; ++Slot) {
            const Uint64 Value = m_WriteValues[Slot].load(std::memory_order_acquire);
            if (Value < OldestValue) {
                OldestSlot  = Slot;
                OldestValue = Value;
            }
        }
        m_WriteValues[OldestSlot].store(m_CompletedTimeline->IncreaseHostValue(), std::memory_order_release);
        return GetSlotOffset(OldestSlot);
    }
    [[nodiscard]] auto GetSlotOffset(Uint32 Slot) const -> Uint64 {
        return static_cast<Uint64>(Slot) * GetSize();
    }

    /// @brief Read the newest completed slot's bytes, if any.
    ///
    /// Readers consume only slots whose stamp the device timeline has passed (newest completed wins). The
    /// completed-value query borrows the resource-context graphics timeline, so the read is valid only while
    /// the resource context is alive.
    [[nodiscard]] auto TryRead() const -> std::vector<std::byte> override {
        const auto Completed = m_CompletedTimeline->GetDeviceValue();

        Uint64 BestValue = 0;
        Uint32 BestSlot  = std::numeric_limits<Uint32>::max();
        for (Uint32 Slot = 0; Slot < m_RingSize; ++Slot) {
            const Uint64 Value = m_WriteValues[Slot].load(std::memory_order_acquire);
            if (Value != 0 && Value <= Completed && Value > BestValue) {
                BestValue = Value;
                BestSlot  = Slot;
            }
        }
        if (BestSlot == std::numeric_limits<Uint32>::max())
            return {};

        std::vector<std::byte> Out(GetSize());
        const Uint64           Offset = GetSlotOffset(BestSlot);
        if (!m_Coherent)
            vmaInvalidateAllocation(m_Allocator, m_Allocation, Offset, Out.size());
        std::memcpy(Out.data(), m_Mapped + Offset, Out.size());
        return Out;
    }

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
