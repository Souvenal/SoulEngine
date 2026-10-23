module;

#include <vk_mem_alloc.h>

export module RHIVulkan.AccelerationStructure;

import Core;
import RHI;
import TaskGraph;
import vulkan;
import std;

import RHIVulkan.Capability;
import RHIVulkan.Context;
import RHIVulkan.Debug;
import RHIVulkan.FrameContext;
import RHIVulkan.Profiling;
import RHIVulkan.RingBuffer;

namespace SoulEngine {

/// @brief One native acceleration structure: a self-owned VMA storage buffer plus the AS handle.
///
/// The storage buffer carries no device address — AS storage is referenced only through the AS handle's own
/// address. The buffer's native destruction rides the backend deferred deletion queue: a virtual timeline
/// event keeps it valid until the submission that referenced it has drained.
export class VulkanNativeAccelerationStructure final {
  public:
    VulkanNativeAccelerationStructure()                                                            = default;
    VulkanNativeAccelerationStructure(const VulkanNativeAccelerationStructure&)                    = delete;
    auto operator=(const VulkanNativeAccelerationStructure&) -> VulkanNativeAccelerationStructure& = delete;
    VulkanNativeAccelerationStructure(VulkanNativeAccelerationStructure&&)                         = delete;
    auto operator=(VulkanNativeAccelerationStructure&&) -> VulkanNativeAccelerationStructure&      = delete;

    ~VulkanNativeAccelerationStructure();

    [[nodiscard]] static auto
    Create(const VulkanResourceContext& Context, vk::AccelerationStructureTypeKHR Type, Uint64 Size, StringView Name)
        -> std::expected<UPtr<VulkanNativeAccelerationStructure>, ErrorMessage>;

    [[nodiscard]] auto Get() const -> vk::AccelerationStructureKHR {
        return *m_AccelerationStructure;
    }

    [[nodiscard]] auto GetAddress() const -> vk::DeviceAddress {
        return m_Address;
    }

  private:
    VulkanDeferredDeletionQueue*       m_DeferredQueue         = nullptr; ///< borrowed; owned by the resource context
    vk::Buffer                         m_Storage               = nullptr;
    VmaAllocation                      m_StorageAllocation     = nullptr;
    vk::raii::AccelerationStructureKHR m_AccelerationStructure = nullptr;
    vk::DeviceAddress                  m_Address               = 0;
};

/// @brief One baked BLAS build request for the AS manager's frame-start batch.
///
/// Everything is snapshotted at descriptor creation: record-time work is a scratch-ring carve plus command
/// emission, never a payload re-resolution or a device call. The ref carries retention and the recorded-flag
/// guard.
export struct VulkanBlasBuildEntry {
    RHIRef<RHIBottomLevelAccelerationStructure> Blas        = {};      ///< retention plus the recorded flag
    vk::AccelerationStructureKHR                Dst         = nullptr; ///< native handle, fixed at creation
    vk::AccelerationStructureGeometryKHR        Geometry    = {};      ///< baked triangles geometry
    vk::AccelerationStructureBuildRangeInfoKHR  Range       = {};      ///< baked build range
    Uint64                                      ScratchSize = 0;       ///< buildScratchSize, creation-time query
};

/// @brief One baked TLAS build request for the AS manager's frame-start batch.
///
/// Instances are packed at descriptor creation (BLAS addresses are immutable once their payloads are
/// created). BlasRefs is parallel to Instances and exists only for the recorded-flag guard at record time.
export struct VulkanTlasBuildEntry {
    RHIRef<RHITopLevelAccelerationStructure>                 Tlas      = {};      ///< retention
    vk::AccelerationStructureKHR                             Dst       = nullptr; ///< native handle, fixed at creation
    std::vector<vk::AccelerationStructureInstanceKHR>        Instances = {};      ///< packed at creation
    std::vector<RHIRef<RHIBottomLevelAccelerationStructure>> BlasRefs  = {};      ///< parallel to Instances
    Uint64                                                   ScratchSize = 0; ///< buildScratchSize, creation-time query
};

/// @brief Vulkan GPU payload for reusable object-space triangle geometry.
///
/// Create does the whole host-side job synchronously on the calling thread — input validation, geometry
/// baking, the build-size query, and native storage/handle/address materialization (VMA and the device-level
/// calls are thread-safe) — so Ready at return means the GPU resource exists. The build itself is GPU work
/// submitted through BuildBlas and recorded by the AS manager's frame-start batch. A static BLAS is built
/// exactly once.
export class VulkanBottomLevelAccelerationStructure final : public RHIBottomLevelAccelerationStructure {
  public:
    explicit VulkanBottomLevelAccelerationStructure(String Name)
        : RHIBottomLevelAccelerationStructure(std::move(Name)) {}

    ~VulkanBottomLevelAccelerationStructure() override = default;

    VulkanBottomLevelAccelerationStructure(const VulkanBottomLevelAccelerationStructure&)                    = delete;
    auto operator=(const VulkanBottomLevelAccelerationStructure&) -> VulkanBottomLevelAccelerationStructure& = delete;

    /// @brief Validate the inputs, bake the geometry, query the build sizes, and materialize the native
    /// storage, handle, and device address — all synchronously on the calling thread.
    [[nodiscard]] static auto
    Create(const VulkanResourceContext& Context, StringView Name, const RHIBottomLevelAccelerationStructureDesc& Desc)
        -> std::expected<UPtr<RHIBottomLevelAccelerationStructure>, ErrorMessage>;

    /// @brief Bake this BLAS's build entry for the manager's pending batch; the ref fills retention and the
    /// recorded-flag guard.
    [[nodiscard]] auto BakeBuildEntry(RHIRef<RHIBottomLevelAccelerationStructure> Self) const -> VulkanBlasBuildEntry;

    /// @brief Whether the frame-start batch has recorded this BLAS's build. Written at record time on the RHI
    /// thread and read by the TLAS guard on the same thread.
    [[nodiscard]] auto IsBuildRecorded() const -> bool {
        return m_IsBuildRecorded;
    }

    /// @brief Stamp the build as recorded; the frame-start batch sets it when the build command is emitted.
    auto MarkBuildRecorded() -> void {
        m_IsBuildRecorded = true;
    }

    [[nodiscard]] auto GetAccelerationStructure() const -> vk::AccelerationStructureKHR {
        return m_Native->Get();
    }

    [[nodiscard]] auto GetAccelerationStructureAddress() const -> vk::DeviceAddress {
        return m_Native->GetAddress();
    }

  private:
    UPtr<VulkanNativeAccelerationStructure>    m_Native          = nullptr;
    vk::AccelerationStructureGeometryKHR       m_Geometry        = {}; ///< baked triangles geometry
    vk::AccelerationStructureBuildRangeInfoKHR m_Range           = {}; ///< baked build range
    Uint64                                     m_ScratchSize     = 0;  ///< buildScratchSize, creation-time query
    bool                                       m_IsBuildRecorded = false;
};

/// @brief Per-frame TLAS: Create packs the instances and materializes native storage sized to the instance
/// count, synchronously on the calling thread; the build is recorded by the AS manager's frame-start phase.
///
/// Every frame creates a fresh TLAS object, so there is no capacity ring: storage lives and dies with this
/// object via deferred deletion, while the build's scratch and instance ranges are bump-allocated out of the
/// AS manager's rings and retire via the rings' frame-seal timeline markers.
export class VulkanTopLevelAccelerationStructure final : public RHITopLevelAccelerationStructure {
  public:
    explicit VulkanTopLevelAccelerationStructure(String Name) : RHITopLevelAccelerationStructure(std::move(Name)) {}

    ~VulkanTopLevelAccelerationStructure() override = default;

    VulkanTopLevelAccelerationStructure(const VulkanTopLevelAccelerationStructure&)                    = delete;
    auto operator=(const VulkanTopLevelAccelerationStructure&) -> VulkanTopLevelAccelerationStructure& = delete;
    VulkanTopLevelAccelerationStructure(VulkanTopLevelAccelerationStructure&&)                         = delete;
    auto operator=(VulkanTopLevelAccelerationStructure&&) -> VulkanTopLevelAccelerationStructure&      = delete;

    /// @brief Validate the instances, pack them, query the build sizes, and materialize the native storage,
    /// handle, and device address — all synchronously on the calling thread.
    [[nodiscard]] static auto
    Create(const VulkanResourceContext& Context, StringView Name, const RHITopLevelAccelerationStructureDesc& Desc)
        -> std::expected<UPtr<RHITopLevelAccelerationStructure>, ErrorMessage>;

    /// @brief Bake this TLAS's build entry for the manager's pending batch; the ref fills retention.
    [[nodiscard]] auto BakeBuildEntry(RHIRef<RHITopLevelAccelerationStructure> Self) const -> VulkanTlasBuildEntry;

    [[nodiscard]] auto GetAccelerationStructure() const -> vk::AccelerationStructureKHR {
        return m_Native->Get();
    }

  private:
    UPtr<VulkanNativeAccelerationStructure>                  m_Native      = nullptr;
    std::vector<vk::AccelerationStructureInstanceKHR>        m_Instances   = {}; ///< packed at creation
    std::vector<RHIRef<RHIBottomLevelAccelerationStructure>> m_BlasRefs    = {}; ///< parallel to m_Instances
    Uint64                                                   m_ScratchSize = 0;  ///< creation-time query
};

/// @brief The single home of acceleration-structure concerns.
///
/// CreateBlas/CreateTlas validate, bake, and materialize the payloads synchronously on the calling thread
/// (VMA and the device-level calls are thread-safe); BuildBlas/BuildTlas enqueue frame-affined tasks
/// (callable from any thread) that append a baked build entry to the manager's pending lists at the RHI
/// thread's frame-task drain. RecordBuildBatch runs the frame-start AS phase — the BLAS batch, then each TLAS
/// build — into its own secondary, carving scratch and instance ranges out of the manager's two rings: a
/// device-local scratch ring and a host-mapped instance ring, both retiring through the ring frame-seal
/// timeline markers. The render device owns one instance and forwards; the pending lists and the build phase
/// are RHI-thread confined.
export class VulkanAccelerationStructureManager final {
  public:
    VulkanAccelerationStructureManager()                                                             = default;
    VulkanAccelerationStructureManager(const VulkanAccelerationStructureManager&)                    = delete;
    auto operator=(const VulkanAccelerationStructureManager&) -> VulkanAccelerationStructureManager& = delete;
    VulkanAccelerationStructureManager(VulkanAccelerationStructureManager&&)                         = default;
    auto operator=(VulkanAccelerationStructureManager&&) -> VulkanAccelerationStructureManager&      = default;

    /// @brief Create the manager with its scratch and instance rings.
    ///
    /// The rings materialize only when hardware ray tracing is available; without it every BLAS/TLAS creation
    /// is rejected at the descriptor level, so the build phase can never run.
    /// @param Context               resource context providing the VMA allocator, device, and debug utils.
    /// @param ScratchRingCapacity   initial scratch ring capacity in bytes (RHI.Vulkan.ASScratchRingSize).
    /// @param InstanceRingCapacity  initial instance ring capacity in bytes (RHI.Vulkan.ASInstanceRingSize).
    [[nodiscard]] static auto
    Create(const VulkanResourceContext& Context, Uint64 ScratchRingCapacity, Uint64 InstanceRingCapacity)
        -> std::expected<VulkanAccelerationStructureManager, ErrorMessage>;

    /// @brief Release the VMA-backed rings while the allocator is still alive.
    ///
    /// Device shutdown ordering requires this to run before the allocator is destroyed.
    auto Shutdown() -> void;

    /// @brief Create and materialize a BLAS synchronously on the calling thread: validation, geometry
    /// baking, the build-size query, and native storage/handle/address allocation are all thread-safe. The
    /// ref publishes Ready at return; the build rides BuildBlas.
    /// @param Name  logical RHI name; becomes the debug-utils object name.
    /// @param Desc  BLAS description; the referenced geometry buffers must be Ready.
    [[nodiscard]] auto CreateBlas(StringView Name, const RHIBottomLevelAccelerationStructureDesc& Desc)
        -> std::expected<RHIRef<RHIBottomLevelAccelerationStructure>, ErrorMessage>;

    /// @brief Create and materialize the per-frame TLAS synchronously on the calling thread: instance
    /// packing, the build-size query, and native storage allocation are all thread-safe. The ref publishes
    /// Ready at return; the build rides BuildTlas.
    /// @param Name  logical RHI name; becomes the debug-utils object name.
    /// @param Desc  TLAS description; every instance must reference a published BLAS payload.
    [[nodiscard]] auto CreateTlas(StringView Name, const RHITopLevelAccelerationStructureDesc& Desc)
        -> std::expected<RHIRef<RHITopLevelAccelerationStructure>, ErrorMessage>;

    /// @brief Build a BLAS: hand a frame-affined task carrying the ref to the RHI thread.
    ///
    /// The task (drained at the matching RHI frame, before Execute) bakes the build entry from the payload
    /// and appends it to the pending list; Execute records the whole batch at frame start, after the upload
    /// batch, so same-frame consumers are safe by submission order. Frame affinity binds the build to the
    /// producer frame; a stale task (its frame never executed) is discarded by the task graph.
    /// @param Blas  BLAS ref; must be Ready at the call site.
    [[nodiscard]] auto BuildBlas(RHIRef<RHIBottomLevelAccelerationStructure> Blas) -> std::expected<void, ErrorMessage>;

    /// @brief Build a per-frame TLAS: hand a frame-affined task carrying the ref to the RHI thread. Same
    /// contract as BuildBlas; the build is recorded after the BLAS batch, ahead of every pass.
    /// @param Tlas  TLAS ref; must be Ready at the call site.
    [[nodiscard]] auto BuildTlas(RHIRef<RHITopLevelAccelerationStructure> Tlas) -> std::expected<void, ErrorMessage>;

    /// @brief Record the drained AS work (the BLAS batch, then each TLAS build) into a dedicated secondary
    /// appended to the primary ahead of every pass, so the passes consume the builds in submission order.
    ///
    /// A no-op when both pending lists are empty. The lists are drained at entry; entries are self-contained
    /// (baked at creation), so record-time work is a ring carve plus command emission — no device calls, no
    /// payload re-resolution beyond the recorded-flag guard. Recorded entries need no further retention:
    /// every ref is already held by its renderer-side owner or a recorded command, and native destruction
    /// rides the backend deferred deletion queue.
    /// @param FC          executing frame slot's context; the secondary rides its ScratchSecondaries.
    /// @param FrameIndex  executing frame slot index, for the secondary's debug name.
    /// @param Primary     the frame's primary command buffer.
    [[nodiscard]] auto RecordBuildBatch(VulkanFrameContext& FC, Uint32 FrameIndex, vk::raii::CommandBuffer& Primary)
        -> std::expected<void, ErrorMessage>;

    /// @brief Tick-chain relay: forward the frame-begin timeline value to both rings.
    /// @param FrameBeginTimelineValue  host-reserved frame-begin value, issued by the device Tick.
    auto Tick(Uint64 FrameBeginTimelineValue) -> void;

  private:
    /// @brief BLAS lane of the frame-start batch: a scratch carve plus one batch entry per request, then ONE
    /// build call and the aggregated barrier. Fail-soft per entry (logged and skipped): the recorded flag
    /// guards double builds, and a ring allocation failure drops that entry alone.
    auto RecordBlasLane(vk::raii::CommandBuffer& CmdBuffer, std::span<VulkanBlasBuildEntry> Entries) -> void;

    /// @brief TLAS lane of the frame-start batch: per entry, validate the referenced BLAS builds (the
    /// recorded flag — the BLAS lane runs first, so a flag set this frame means submission order covers this
    /// build), carve scratch and instance ranges, copy the packed instances, build, and seal with a
    /// build-write to ray-tracing-shader-read barrier. Fail-soft per entry.
    auto RecordTlasLane(vk::raii::CommandBuffer& CmdBuffer, std::span<VulkanTlasBuildEntry> Entries) -> void;

    /// @brief vkCmdBuildAccelerationStructuresKHR requires the instance-data address to be 16-byte aligned.
    static constexpr Uint64 kTlasInstanceDataAlignment = 16;

    const VulkanResourceContext*      m_Context             = nullptr; ///< borrowed; outlives the manager
    VulkanRingBuffer                  m_ScratchRing         = {};      ///< device-local build scratch ring
    VulkanRingBuffer                  m_InstanceRing        = {};      ///< host-mapped TLAS instance ring
    bool                              m_IsRayTracingEnabled = false;   ///< rings materialize only when RT is available
    /// @brief Baked BLAS build entries queued by frame-affined tasks; drained by RecordBuildBatch.
    std::vector<VulkanBlasBuildEntry> m_PendingBlasBuilds   = {};
    /// @brief Baked TLAS build entries queued by frame-affined tasks; drained by RecordBuildBatch.
    std::vector<VulkanTlasBuildEntry> m_PendingTlasBuilds   = {};
};

} // namespace SoulEngine
