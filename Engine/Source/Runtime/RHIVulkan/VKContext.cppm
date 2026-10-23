/// @file   RHIVulkan/VKContext.cppm
/// @brief  Vulkan device services and shared image-state tracking.

module;

// volk.h must precede every other Vulkan header in this fragment: it defines VK_NO_PROTOTYPES itself before
// pulling in vulkan_core.h, and errors out if vulkan.h was already included without it. Dropping the raw C
// prototypes is safe here — every call goes through Vulkan-Hpp's dynamic dispatcher or VMA's dynamically
// fetched function table.
#include <volk.h>
#include <vk_mem_alloc.h>
#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>

export module RHIVulkan.Context;

import Core;
import vulkan;
import std;

import RHIVulkan.Capability;
import RHIVulkan.Debug;
import RHIVulkan.SurfaceProvider;
import RHIVulkan.Semaphore;
import WindowSystem;

namespace SoulEngine {

/// @brief Tracked Vulkan image subresource state used for automatic synchronization barriers.
export struct VulkanImageState {
    vk::PipelineStageFlags2 stage       = vk::PipelineStageFlagBits2::eNone;
    vk::AccessFlags2        access      = vk::AccessFlagBits2::eNone;
    vk::ImageLayout         layout      = vk::ImageLayout::eUndefined;
    Uint32                  queueFamily = vk::QueueFamilyIgnored;
    bool                    isWrite     = false;
};

/// @brief Per-subresource tracking key: (image, single aspect bit, mip level, array layer).
export struct VulkanImageSubresourceKey {
    vk::Image               Image      = nullptr;
    vk::ImageAspectFlagBits Aspect     = vk::ImageAspectFlagBits::eColor;
    Uint32                  MipLevel   = 0;
    Uint32                  ArrayLayer = 0;

    bool operator==(const VulkanImageSubresourceKey&) const = default;
};

export struct VulkanImageSubresourceKeyHash {
    auto operator()(const VulkanImageSubresourceKey& Key) const -> std::size_t;
};

/// @brief Device-level per-subresource image state tracker shared by Vulkan command recording.
///
/// State is keyed by (image, aspect, mip, layer); barrier granularity matches the key — no whole-image
/// over-sync, no coalescing (all engine images have arrayLayers == 1 and at most ~13 mips, so per-subresource
/// barrier counts stay small). Ranges are always concrete: every call site resolves subresource counts from
/// the payload it already holds, so the tracker stores no image metadata and creation never touches it.
/// RHI-thread confined — Seed/Forget/Transition all run there.
export class VulkanImageTracker {
  public:
    VulkanImageTracker() = default;

    /// @brief Seed every (color, mip, layer) subresource with State.
    ///
    /// Only for images whose initial state is not Undefined (swapchain images seed their transfer stage); a
    /// default-constructed state needs no seeding — Transition's missing-key default is already Undefined.
    /// Depth/stencil aspect keys are created lazily by Transition.
    auto Seed(vk::Image Image, Uint32 MipLevels, Uint32 ArrayLayers, VulkanImageState State) -> void;

    /// @brief Drop a destroyed image's tracked state.
    ///
    /// Without this, a driver-reused VkImage handle would make the new image inherit a stale layout and emit
    /// an invalid oldLayout barrier (pool era: eviction destroys idle images while their entries would
    /// otherwise linger).
    auto Forget(vk::Image Image) -> void;

    /// @brief Drive every subresource in Range to DesiredState.
    ///
    /// Emits one image memory barrier per subresource whose tracked state differs (or after a prior write).
    /// Range must be concrete — no vk::RemainingMipLevels/vk::RemainingArrayLayers.
    auto Transition(const vk::raii::CommandBuffer&  CmdBuf,
                    vk::Image                       Image,
                    const vk::ImageSubresourceRange Range,
                    const VulkanImageState&         DesiredState) -> void;

  private:
    std::unordered_map<VulkanImageSubresourceKey, VulkanImageState, VulkanImageSubresourceKeyHash> m_SubresourceStates =
        {};
};

/// @brief Tracked Vulkan buffer range state used for automatic synchronization barriers.
///
/// Buffers have no layout; only the pipeline stage, access mask, and write hazard flag are tracked.
export struct VulkanBufferState {
    vk::PipelineStageFlags2 stage   = vk::PipelineStageFlagBits2::eNone;
    vk::AccessFlags2        access  = vk::AccessFlagBits2::eNone;
    bool                    isWrite = false;
};

/// @brief Device-level buffer state tracker shared by Vulkan command recording.
///
/// State is keyed by (buffer, offset). Transient belt slices are disjoint within a frame and an in-flight
/// frame's ranges stay ring-owned until the timeline retires them, so keys never collide with another slice's
/// live range; persistent buffers simply always use offset 0. The tracker is Reset() at every BeginFrame: the
/// frame-slot fence guarantees the previous occupant's GPU work has completed, so a range's first use within
/// a frame needs no barrier and is registered directly.
export class VulkanBufferTracker {
  public:
    VulkanBufferTracker() = default;

    /// @brief Drop all tracked range states.
    ///
    /// Must be called once per frame before new transient allocations are recorded.
    auto Reset() -> void {
        m_BufferStates.clear();
    }

    /// @brief Drive the buffer range [Offset, Offset+Size) to DesiredState.
    ///
    /// Emits a buffer memory barrier when the tracked state differs. A WAR hazard (read -> write) needs an
    /// execution dependency, and any previous write (WAW/RAW) forces a barrier just like the image tracker.
    auto Transition(vk::raii::CommandBuffer& CmdBuf,
                    vk::Buffer               Buffer,
                    vk::DeviceSize           Offset,
                    vk::DeviceSize           Size,
                    const VulkanBufferState& DesiredState) -> void;

  private:
    struct Key {
        vk::Buffer     Buffer = nullptr;
        vk::DeviceSize Offset = 0;

        auto operator==(const Key&) const -> bool = default;
    };

    struct KeyHash {
        auto operator()(const Key& K) const noexcept -> std::size_t;
    };

    std::unordered_map<Key, VulkanBufferState, KeyHash> m_BufferStates = {};
};

/// @brief Backend-owned deferred destruction queue for VMA resources (buffers and images).
///
/// Destruction is treated as a virtual timeline event: Enqueue stamps the entry with a fresh host value
/// (IncreaseHostValue) that no submission ever signals directly — the monotonic device counter covers it once
/// the NEXT real submission completes. No new command can reference a resource after its last ref is gone, so
/// the destruction frame's own submission is an upper bound on its last GPU use, and "covered by the next
/// submission" is always safe. Each lane's entries arrive in non-decreasing stamp order, so retirement pops
/// from the front.
///
/// Distinct from the RHI-level deferred deletion queue: that one is the cross-thread transport (any thread ->
/// RHI thread); this one is the GPU-retirement hop (RHI thread -> GPU completion). Tick() retires entries the
/// device timeline has passed; Flush() destroys unconditionally (shutdown, after the device is idle).
/// RHI-thread confined — buffer destructors only run there; the stamp itself comes from an atomic counter.
export class VulkanDeferredDeletionQueue {
  public:
    VulkanDeferredDeletionQueue() = default;
    /// Safety net: the proper flush point is device Shutdown after the device is idle.
    ~VulkanDeferredDeletionQueue() {
        Flush();
    }

    VulkanDeferredDeletionQueue(const VulkanDeferredDeletionQueue&)                    = delete;
    auto operator=(const VulkanDeferredDeletionQueue&) -> VulkanDeferredDeletionQueue& = delete;
    VulkanDeferredDeletionQueue(VulkanDeferredDeletionQueue&&)                         = delete;
    auto operator=(VulkanDeferredDeletionQueue&&) -> VulkanDeferredDeletionQueue&      = delete;

    /// One dying VMA buffer: the handles destruction needs, plus its retirement stamp.
    struct BufferEntry {
        Uint64        RetireValue = 0;       ///< virtual event value; retired once the device timeline passes it
        vk::Buffer    Buffer      = nullptr; ///< native buffer handle
        VmaAllocation Allocation  = nullptr; ///< VMA allocation destroyed together with the handle
    };

    /// One dying VMA image: the handles destruction needs, plus its retirement stamp.
    struct ImageEntry {
        Uint64        RetireValue = 0;       ///< virtual event value; retired once the device timeline passes it
        vk::Image     Image       = nullptr; ///< native image handle
        VmaAllocation Allocation  = nullptr; ///< VMA allocation destroyed together with the handle
    };

    /// Bind the allocator and timeline the queue destroys and stamps through.
    auto Init(VmaAllocator Allocator, VulkanTimelineSemaphore& Timeline) -> void;

    /// @brief Queue a dying buffer, stamped with a fresh virtual event value.
    ///
    /// The handles stay valid until Tick() retires the entry, so a same-frame consumer that snapshotted the
    /// raw handle (the frame-start upload batch) records safely.
    auto Enqueue(vk::Buffer Buffer, VmaAllocation Allocation) -> void {
        m_BufferEntries.push_back(
            BufferEntry{.RetireValue = m_Timeline->IncreaseHostValue(), .Buffer = Buffer, .Allocation = Allocation});
    }

    /// @brief Queue a dying image, stamped with a fresh virtual event value. Same contract as the
    /// buffer overload: the frame-start upload batch's image lane snapshots raw image handles.
    auto Enqueue(vk::Image Image, VmaAllocation Allocation) -> void {
        m_ImageEntries.push_back(
            ImageEntry{.RetireValue = m_Timeline->IncreaseHostValue(), .Image = Image, .Allocation = Allocation});
    }

    /// Retire every entry whose stamp the device timeline has passed.
    auto Tick() -> void;

    /// Destroy every remaining entry regardless of stamps. Shutdown only, after the device is idle.
    auto Flush() -> void;

  private:
    VmaAllocator             m_Allocator     = nullptr; ///< borrowed from the resource context
    VulkanTimelineSemaphore* m_Timeline      = nullptr; ///< borrowed graphics timeline
    std::deque<BufferEntry>  m_BufferEntries = {};
    std::deque<ImageEntry>   m_ImageEntries  = {};
};

/// Owns Vulkan objects and services shared by backend resource factories.
export class VulkanResourceContext final {
  public:
    VulkanResourceContext() = default;

    ~VulkanResourceContext();

    VulkanResourceContext(const VulkanResourceContext&)                    = delete;
    auto operator=(const VulkanResourceContext&) -> VulkanResourceContext& = delete;
    VulkanResourceContext(VulkanResourceContext&&)                         = delete;
    auto operator=(VulkanResourceContext&&) -> VulkanResourceContext&      = delete;

    [[nodiscard]] static auto Create(IWindowSystem* WindowSys, Uint32 FramesInFlight)
        -> std::expected<UPtr<VulkanResourceContext>, ErrorMessage>;

    [[nodiscard]] auto GetInstance() const -> vk::raii::Instance& {
        return const_cast<vk::raii::Instance&>(m_Instance);
    }
    [[nodiscard]] auto GetSurface() const -> vk::raii::SurfaceKHR& {
        return const_cast<vk::raii::SurfaceKHR&>(m_Surface);
    }
    [[nodiscard]] auto GetPhysicalDevice() const -> vk::raii::PhysicalDevice& {
        return const_cast<vk::raii::PhysicalDevice&>(m_PhysicalDevice);
    }
    [[nodiscard]] auto GetDevice() const -> vk::raii::Device& {
        return const_cast<vk::raii::Device&>(m_Device);
    }
    [[nodiscard]] auto GetDebugUtils() const -> VulkanDebugUtils& {
        return const_cast<VulkanDebugUtils&>(m_DebugUtils);
    }
    /// Create the Tracy GPU profiling context after the device and queues exist.
    auto CreateTracyCtx() -> void;

    auto DestroyTracyCtx() noexcept -> void;

    [[nodiscard]] auto GetTracyCtx() const noexcept -> tracy::VkCtx* {
        return m_TracyCtx;
    }

    [[nodiscard]] auto GetGraphicsQueue() const -> vk::raii::Queue& {
        return const_cast<vk::raii::Queue&>(m_GraphicsQueue);
    }
    [[nodiscard]] auto GetComputeQueue() const -> vk::raii::Queue& {
        return const_cast<vk::raii::Queue&>(m_ComputeQueue);
    }
    [[nodiscard]] auto GetTransferQueue() const -> vk::raii::Queue& {
        return const_cast<vk::raii::Queue&>(m_TransferQueue);
    }
    [[nodiscard]] auto GetImageTracker() const -> VulkanImageTracker& {
        return const_cast<VulkanImageTracker&>(m_ImageTracker);
    }
    [[nodiscard]] auto GetBufferTracker() const -> VulkanBufferTracker& {
        return const_cast<VulkanBufferTracker&>(m_BufferTracker);
    }
    [[nodiscard]] auto GetAllocator() const -> VmaAllocator {
        return m_Allocator;
    }
    [[nodiscard]] auto GetSurfaceProvider() const -> IVulkanSurfaceProvider& {
        return *m_SurfaceProvider;
    }
    [[nodiscard]] auto GetGraphicsFamily() const -> Uint32 {
        return m_GraphicsFamily;
    }
    [[nodiscard]] auto GetComputeFamily() const -> Uint32 {
        return m_ComputeFamily;
    }
    [[nodiscard]] auto GetTransferFamily() const -> Uint32 {
        return m_TransferFamily;
    }
    [[nodiscard]] auto GetFramesInFlight() const -> Uint32 {
        return m_FramesInFlight;
    }
    /// @brief Current frame slot being executed, written by the render device in BeginFrame.
    ///
    /// RHI-thread confined: written and read only while executing that thread's frame, so a plain value
    /// suffices. Frame-slotted payloads (e.g. the TLAS) resolve their internal slot from it.
    [[nodiscard]] auto GetCurrentFrameIndex() const -> Uint32 {
        return m_CurrentFrameIndex;
    }
    auto SetCurrentFrameIndex(Uint32 FrameIndex) -> void {
        m_CurrentFrameIndex = FrameIndex;
    }
    [[nodiscard]] auto GetTimeline() const -> VulkanTimelineSemaphore& {
        return const_cast<VulkanTimelineSemaphore&>(m_Timeline);
    }
    [[nodiscard]] auto GetDeferredDeletionQueue() const -> VulkanDeferredDeletionQueue& {
        return const_cast<VulkanDeferredDeletionQueue&>(m_DeferredDeletionQueue);
    }

  private:
    [[nodiscard]] auto CreateInstance(vk::raii::Context& Context) -> std::expected<void, ErrorMessage>;

    [[nodiscard]] auto PickPhysicalDevice() -> std::expected<void, ErrorMessage>;

    [[nodiscard]] auto ResolveQueueFamilies(std::span<const vk::QueueFamilyProperties2> QueueProps)
        -> std::expected<void, ErrorMessage>;

    [[nodiscard]] auto CreateLogicalDevice() -> std::expected<void, ErrorMessage>;

    [[nodiscard]] auto CreateVMA(vk::raii::Context& Context) -> std::expected<void, ErrorMessage>;

    // Global descriptor manager
    UPtr<IVulkanSurfaceProvider> m_SurfaceProvider = nullptr;
    vk::raii::Instance           m_Instance        = nullptr;
    vk::raii::SurfaceKHR         m_Surface         = nullptr;
    vk::raii::PhysicalDevice     m_PhysicalDevice  = nullptr;
    vk::raii::Device             m_Device          = nullptr;
    tracy::VkCtx*                m_TracyCtx        = nullptr;
    VulkanDebugUtils             m_DebugUtils;
    Uint32                       m_GraphicsFamily = vk::QueueFamilyIgnored;
    Uint32                       m_ComputeFamily  = vk::QueueFamilyIgnored;
    Uint32                       m_TransferFamily = vk::QueueFamilyIgnored;
    vk::raii::Queue              m_GraphicsQueue  = nullptr;
    vk::raii::Queue              m_ComputeQueue   = nullptr;
    vk::raii::Queue              m_TransferQueue  = nullptr;
    VmaAllocator                 m_Allocator      = nullptr;
    VulkanImageTracker           m_ImageTracker;
    VulkanBufferTracker          m_BufferTracker;
    VulkanTimelineSemaphore      m_Timeline;
    VulkanDeferredDeletionQueue  m_DeferredDeletionQueue;
    Uint32                       m_FramesInFlight    = 2;
    Uint32                       m_CurrentFrameIndex = 0;
};

} // namespace SoulEngine
