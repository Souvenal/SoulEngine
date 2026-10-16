module;

#include <vk_mem_alloc.h>

export module Vulkan:RingBuffer;

import Core;
import vulkan;
import std;

import :Context;
import :Debug;

namespace SoulEngine {

/// Memory placement of a ring's backing instances; decides whether ranges are host-writable.
enum class VulkanRingMemoryClass : Uint8 {
    Unknown,
    /// Host-visible, host-coherent, persistently mapped; callers write ranges through MappedData.
    HostMapped,
    /// Device-local GPU working memory; consumers use the range's buffer handle and offset.
    DeviceLocal,
};

/// @brief One bump-allocation result out of a VulkanRingBuffer: a ring-owned buffer handle plus the range.
///
/// Marker/stamp retirement keeps the backing instance alive until the GPU has consumed every copy recorded
/// against the range. HostMapped instances are persistently mapped, so the caller writes the range directly
/// through MappedData; memory is contractually host-coherent, no flush is needed. DeviceLocal instances leave
/// MappedData null.
struct RingAllocation {
    vk::Buffer        Buffer     = nullptr; ///< backing instance's buffer; non-owning, retired by the ring
    std::byte*        MappedData = nullptr; ///< persistently mapped host pointer to the range start (HostMapped only)
    Uint64            Offset     = 0;       ///< range start within the buffer
    Uint64            Size       = 0;       ///< range size in bytes (unpadded)
    /// Device address of the range start; 0 when the ring's usage lacks eShaderDeviceAddress.
    vk::DeviceAddress DeviceAddress = 0;
};

/// @brief Wrap-around bump-allocation ring over VMA buffers.
///
/// Usage: Create() once with a memory class, then per frame Allocate() ranges and call Tick() at frame begin
/// so GPU-consumed ranges are recycled. On a HostMapped ring Allocate() returns a RingAllocation with a
/// persistently mapped host pointer — the caller writes the bytes directly (host-coherent, no flush); a
/// DeviceLocal ring hands out GPU-only ranges consumed by device address. Sizing is a starting budget, not a
/// limit: the ring grows on demand. RHI-thread confined.
class VulkanRingBuffer {
  public:
    VulkanRingBuffer()                                               = default;
    VulkanRingBuffer(const VulkanRingBuffer&)                        = delete;
    auto operator=(const VulkanRingBuffer&) -> VulkanRingBuffer&     = delete;
    VulkanRingBuffer(VulkanRingBuffer&&) noexcept                    = default;
    auto operator=(VulkanRingBuffer&&) noexcept -> VulkanRingBuffer& = default;

    /// @brief Create the ring with its initial backing instance.
    ///
    /// The initial capacity is the whole budget until the first growth — a zero capacity is a configuration
    /// error, not a grow trigger.
    /// @param Context      resource context providing the VMA allocator, debug utils, and the device
    ///                     timeline.
    /// @param Name         debug name applied to every backing instance (duplicates are legal).
    /// @param Usage        native usage union every backing instance is created with (e.g. eTransferSrc).
    /// @param Capacity     initial instance capacity in bytes; must be non-zero.
    /// @param Alignment    allocation alignment in bytes; power of two, at least kMinAlignment. The default
    ///                     is the vkCmdCopyBuffer offset floor; raise it only when placements must satisfy
    ///                     a stricter device limit.
    /// @param MemoryClass  backing-instance memory placement; HostMapped instances are persistently
    ///                     mapped and verified host-coherent, DeviceLocal instances are GPU-only.
    [[nodiscard]] static auto Create(const VulkanResourceContext& Context,
                                     StringView                   Name,
                                     vk::BufferUsageFlags         Usage,
                                     Uint64                       Capacity,
                                     Uint64                       Alignment   = kMinAlignment,
                                     VulkanRingMemoryClass        MemoryClass = VulkanRingMemoryClass::HostMapped)
        -> std::expected<VulkanRingBuffer, ErrorMessage> {
        if (!Usage)
            return std::unexpected(ErrorMessage("VulkanRingBuffer::Create: usage is empty"));
        if (Capacity == 0)
            return std::unexpected(ErrorMessage("VulkanRingBuffer::Create: capacity must be greater than zero"));
        if (Alignment < kMinAlignment || (Alignment & (Alignment - 1)) != 0)
            return std::unexpected(ErrorMessage(
                Format("VulkanRingBuffer::Create: alignment must be a power of two and at least {} (got {})",
                       kMinAlignment,
                       Alignment)));
        if (MemoryClass == VulkanRingMemoryClass::Unknown)
            return std::unexpected(ErrorMessage("VulkanRingBuffer::Create: memory class is Unknown"));
        VulkanRingBuffer Ring;
        Ring.m_Context     = &Context;
        Ring.m_Name        = String(Name);
        Ring.m_Usage       = Usage;
        Ring.m_Alignment   = Alignment;
        Ring.m_MemoryClass = MemoryClass;
        auto Instance      = Ring.CreateInstance(Capacity);
        if (!Instance)
            return std::unexpected(Instance.error().Append("VulkanRingBuffer::Create: initial buffer creation failed"));
        Ring.m_Current = std::move(*Instance);
        return Ring;
    }

    /// @brief Bump-allocate a range in the current instance.
    ///
    /// Wraps the head past retired space; grows the ring when no placement fits — a larger instance takes
    /// over new allocations while the superseded one is stamped at the next frame-begin seal and destroyed by
    /// Tick() once the timeline passes the stamp (grow-only, never shrinks). Head == Tail means EMPTY, so a
    /// placement that would exactly fill the ring diverts to growth and the cursors never sit on a full ring.
    [[nodiscard]] auto Allocate(Uint64 Size) -> std::expected<RingAllocation, ErrorMessage> {
        if (Size == 0)
            return std::unexpected(ErrorMessage("VulkanRingBuffer::Allocate: size must be greater than zero"));
        if (Size > std::numeric_limits<Uint64>::max() - (m_Alignment - 1))
            return std::unexpected(ErrorMessage("VulkanRingBuffer::Allocate: size alignment overflow"));
        const Uint64 Aligned = (Size + m_Alignment - 1) / m_Alignment * m_Alignment;

        for (;;) {
            Instance&    I        = *m_Current;
            const Uint64 Capacity = I.Capacity;
            // Strict: a slice exactly as large as the instance could only be placed exactly full — divert it
            // to growth like any exact fit.
            if (Aligned < Capacity) {
                if (I.Head == I.Tail) {
                    // Empty instance: head room first, then wrap to the front.
                    if (Aligned <= Capacity - I.Head) {
                        if (auto S = TryPlace(I, Aligned, Size, I.Head))
                            return *S;
                    } else if (auto S = TryPlace(I, Aligned, Size, 0)) {
                        return *S;
                    }
                } else if (I.Head < I.Tail) {
                    // Free space is the contiguous retired span ahead of the head.
                    if (Aligned <= I.Tail - I.Head) {
                        if (auto S = TryPlace(I, Aligned, Size, I.Head))
                            return *S;
                    }
                } else {
                    // Head room first; otherwise wrap into the retired front span.
                    if (Aligned <= Capacity - I.Head) {
                        if (auto S = TryPlace(I, Aligned, Size, I.Head))
                            return *S;
                    }
                    if (Aligned <= I.Tail) {
                        if (auto S = TryPlace(I, Aligned, Size, 0))
                            return *S;
                    }
                }
            }
            // No placement fits: grow. The new capacity must be strictly larger than the request — an exact
            // fit would divert again. The superseded instance is stamped at the next frame-begin seal and
            // destroyed by Tick() once the device timeline passes the stamp.
            Uint64 NewCapacity = Aligned + m_Alignment;
            if (Capacity <= std::numeric_limits<Uint64>::max() / 2)
                NewCapacity = std::max(Capacity * 2, NewCapacity);
            auto Grown = CreateInstance(NewCapacity);
            if (!Grown)
                return std::unexpected(Grown.error().Append("VulkanRingBuffer::Allocate: growth allocation failed"));
            LogInfo("VulkanRingBuffer: growing from {} to {} bytes", Capacity, NewCapacity);
            m_InstancesToRetire.push_back(std::move(m_Current));
            m_Current = std::move(*Grown);
        }
    }

    /// @brief Frame-begin retirement and seal: retire first, then seal the previous frame's pending
    /// allocations against the frame-begin timeline value.
    ///
    /// Step order is deliberate — retiring first lets the new frame's allocations reuse the freed ranges.
    /// FrameBeginTimelineValue is host-reserved (IncreaseHostValue): strictly greater than every submitted
    /// value so far and strictly less than the next submission's signal value, so markers stamped with it
    /// retire once the next real submission completes. Sealing here marks the previous frame's end, so
    /// recycling lands one frame later than stamping at the submission itself. Every superseded instance is
    /// flushed into the stamped-retirement list with the same value — their last-consuming copies all ride an
    /// earlier submission. The seal is a no-op for frames without allocations.
    /// @param FrameBeginTimelineValue  host-reserved frame-begin value, issued by the device Tick.
    auto Tick(Uint64 FrameBeginTimelineValue) -> void {
        const Uint64 Completed = m_Context->GetTimeline().GetDeviceValue();

        // step 1: delete used-up instances
        std::erase_if(m_RetiredInstances,
                      [Completed](const RetiredInstance& R) { return R.TimelineValue <= Completed; });

        if (!m_Current)
            return;
        Instance& I = *m_Current;

        // step 2: give back used-up ranges in current instance
        while (!I.Markers.empty() && I.Markers.front().TimelineValue <= Completed) {
            I.Tail = I.Markers.front().Offset;
            I.Markers.pop_front();
        }

        // step 3: seal the previous frame's pending allocations and superseded instances
        if (I.IsDirty) {
            I.Markers.push_back(RingMarker{.TimelineValue = FrameBeginTimelineValue, .Offset = I.Head});
            I.IsDirty = false;
        }
        for (auto& Inst : m_InstancesToRetire)
            m_RetiredInstances.push_back(
                RetiredInstance{.TimelineValue = FrameBeginTimelineValue, .Instance = std::move(Inst)});
        m_InstancesToRetire.clear();
    }

  private:
    /// @brief One frame-seal retirement marker.
    ///
    /// All allocations below Offset (ring order) are GPU-consumed once the timeline passes TimelineValue.
    struct RingMarker {
        Uint64 TimelineValue = 0; ///< frame-begin timeline value stamped at the seal
        Uint64 Offset        = 0; ///< head cursor at seal time
    };

    /// @brief One backing buffer instance: a self-owned VMA buffer plus its ring cursors.
    ///
    /// Instances are heap-pinned (UPtr only, never moved) so slice buffer handles stay stable across growth.
    struct Instance {
        Instance() = default;
        ~Instance() {
            if (Allocation)
                vmaDestroyBuffer(Allocator, static_cast<VkBuffer>(Buffer), Allocation);
        }
        Instance(const Instance&)                    = delete;
        auto operator=(const Instance&) -> Instance& = delete;
        Instance(Instance&&)                         = delete;
        auto operator=(Instance&&) -> Instance&      = delete;

        VmaAllocator      Allocator   = nullptr; ///< borrowed from the resource context
        vk::Buffer        Buffer      = nullptr;
        VmaAllocation     Allocation  = nullptr;
        std::byte*        Mapped      = nullptr; ///< persistent map of the whole instance (HostMapped only)
        /// @brief Base device address.
        ///
        /// Queried once at creation when the ring's usage carries eShaderDeviceAddress, 0 otherwise.
        vk::DeviceAddress BaseAddress = 0;
        Uint64            Capacity    = 0;
        Uint64            Head = 0; ///< next bump offset; Head == Tail means empty (exact fills divert to growth)
        Uint64            Tail = 0; ///< oldest unretired offset
        bool IsDirty = false; ///< allocated since the last frame-begin seal (only meaningful on the current instance)
        std::deque<RingMarker> Markers = {}; ///< frame-seal stamps; only the current instance accumulates these
    };

    /// @brief A superseded instance stamped at the frame-begin seal following its last use.
    ///
    /// Destroyed by Tick() once the device timeline passes the stamp — superseded instances receive no new
    /// allocations, so no finer tracking is needed.
    struct RetiredInstance {
        Uint64         TimelineValue = 0;
        UPtr<Instance> Instance      = nullptr;
    };

    /// @brief Allocate one backing instance with the ring's usage and memory class. HostMapped
    /// instances are persistently mapped and verified host-coherent; DeviceLocal instances stay GPU-only.
    [[nodiscard]] auto CreateInstance(Uint64 Capacity) -> std::expected<UPtr<Instance>, ErrorMessage> {
        auto Out       = std::make_unique<Instance>();
        Out->Allocator = m_Context->GetAllocator();
        Out->Capacity  = Capacity;

        const bool                 IsHostMapped = m_MemoryClass == VulkanRingMemoryClass::HostMapped;
        const vk::BufferCreateInfo BufCI{
            .size        = Capacity,
            .usage       = m_Usage,
            .sharingMode = vk::SharingMode::eExclusive,
        };
        const VmaAllocationCreateInfo AllocInfo{
            .flags = IsHostMapped ? static_cast<VmaAllocationCreateFlags>(
                                        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                        VMA_ALLOCATION_CREATE_MAPPED_BIT)
                                  : static_cast<VmaAllocationCreateFlags>(0),
            .usage = IsHostMapped ? VMA_MEMORY_USAGE_AUTO : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        };
        VmaAllocationInfo AllocationInfo{};
        if (vmaCreateBuffer(m_Context->GetAllocator(),
                            reinterpret_cast<const VkBufferCreateInfo*>(&BufCI),
                            &AllocInfo,
                            reinterpret_cast<VkBuffer*>(&Out->Buffer),
                            &Out->Allocation,
                            &AllocationInfo) != VK_SUCCESS)
            return std::unexpected(ErrorMessage("VulkanRingBuffer: VMA buffer creation failed"));
        if (IsHostMapped) {
            if (!AllocationInfo.pMappedData)
                return std::unexpected(ErrorMessage("VulkanRingBuffer: allocation is not persistently mapped"));
            Out->Mapped = static_cast<std::byte*>(AllocationInfo.pMappedData);

            // Contractual requirement: ring writes rely on implicit host-write visibility at submission —
            // there is no HOST barrier anywhere.
            VkMemoryPropertyFlags Props = 0;
            vmaGetAllocationMemoryProperties(m_Context->GetAllocator(), Out->Allocation, &Props);
            if ((Props & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
                return std::unexpected(ErrorMessage("VulkanRingBuffer: ring memory is not host-coherent"));
        }

        // Cache the base device address once: per-allocation address queries would be one device call per
        // range.
        if (m_Usage & vk::BufferUsageFlagBits::eShaderDeviceAddress)
            Out->BaseAddress =
                m_Context->GetDevice().getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = Out->Buffer});

        m_Context->GetDebugUtils().SetObjectName(Out->Buffer, m_Name);
        return Out;
    }

    /// @brief Place a range at At after the caller has confirmed the room.
    ///
    /// Returns nullopt when the placement would land exactly on the tail: that fills the ring, and Head ==
    /// Tail means empty — exact fits divert to growth.
    [[nodiscard]] auto TryPlace(Instance& I, Uint64 Aligned, Uint64 Size, Uint64 At) -> std::optional<RingAllocation> {
        const Uint64 NewHead = (At + Aligned) % I.Capacity;
        if (NewHead == I.Tail)
            return std::nullopt;
        const RingAllocation Result{.Buffer        = I.Buffer,
                                    .MappedData    = I.Mapped ? I.Mapped + At : nullptr,
                                    .Offset        = At,
                                    .Size          = Size,
                                    .DeviceAddress = I.BaseAddress == 0 ? 0 : I.BaseAddress + At};
        I.Head    = NewHead;
        I.IsDirty = true;
        return Result;
    }

    const VulkanResourceContext* m_Context = nullptr; ///< borrowed; outlives the ring
    String                       m_Name    = {};      ///< debug name applied to every instance
    vk::BufferUsageFlags         m_Usage   = {};      ///< native usage union for every instance
    UPtr<Instance>               m_Current = nullptr; ///< active bump target
    std::vector<UPtr<Instance>>  m_InstancesToRetire =
        {}; ///< superseded, awaiting a timeline stamp at the frame-begin seal
    std::vector<RetiredInstance> m_RetiredInstances = {}; ///< stamped, destroyed by Tick once the timeline passes
    Uint64                       m_Alignment        = kMinAlignment; ///< allocation alignment for every placement
    VulkanRingMemoryClass m_MemoryClass = VulkanRingMemoryClass::HostMapped; ///< backing-instance memory placement

  public:
    static constexpr Uint64 kMinAlignment = 4; ///< alignment floor: vkCmdCopyBuffer source offsets
};

} // namespace SoulEngine
