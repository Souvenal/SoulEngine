module;

#include <vk_mem_alloc.h>

export module RHIVulkan.RingBuffer;

import Core;
import vulkan;
import std;

import RHIVulkan.Context;
import RHIVulkan.Debug;

namespace SoulEngine {

/// Memory placement of a ring's backing instances; decides whether ranges are host-writable.
export enum class VulkanRingMemoryClass : Uint8 {
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
export struct RingAllocation {
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
export class VulkanRingBuffer {
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
        -> std::expected<VulkanRingBuffer, ErrorMessage>;

    /// @brief Bump-allocate a range in the current instance.
    ///
    /// Wraps the head past retired space; grows the ring when no placement fits — a larger instance takes
    /// over new allocations while the superseded one is stamped at the next frame-begin seal and destroyed by
    /// Tick() once the timeline passes the stamp (grow-only, never shrinks). Head == Tail means EMPTY, so a
    /// placement that would exactly fill the ring diverts to growth and the cursors never sit on a full ring.
    [[nodiscard]] auto Allocate(Uint64 Size) -> std::expected<RingAllocation, ErrorMessage>;

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
    auto Tick(Uint64 FrameBeginTimelineValue) -> void;

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
        ~Instance();
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
    [[nodiscard]] auto CreateInstance(Uint64 Capacity) -> std::expected<UPtr<Instance>, ErrorMessage>;

    /// @brief Place a range at At after the caller has confirmed the room.
    ///
    /// Returns nullopt when the placement would land exactly on the tail: that fills the ring, and Head ==
    /// Tail means empty — exact fits divert to growth.
    [[nodiscard]] auto TryPlace(Instance& I, Uint64 Aligned, Uint64 Size, Uint64 At) -> std::optional<RingAllocation>;

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
