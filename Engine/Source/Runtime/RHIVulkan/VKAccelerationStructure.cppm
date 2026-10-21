module;

#include <hlsl++.h>
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

/// @brief Pack a row-vector hlslpp world transform into Vulkan's row-major 3x4.
///
/// The upper-left 3x3 block is transposed, the fourth row becomes translation.
export [[nodiscard]] auto ToVkInstanceTransform(const hlslpp::float4x4& Matrix) -> vk::TransformMatrixKHR {
    return vk::TransformMatrixKHR{std::array<std::array<Float32, 4>, 3>{
        std::array<Float32, 4>{Matrix[0].x, Matrix[1].x, Matrix[2].x, Matrix[3].x},
        std::array<Float32, 4>{Matrix[0].y, Matrix[1].y, Matrix[2].y, Matrix[3].y},
        std::array<Float32, 4>{Matrix[0].z, Matrix[1].z, Matrix[2].z, Matrix[3].z},
    }};
}

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

    ~VulkanNativeAccelerationStructure() {
        if (m_StorageAllocation)
            m_DeferredQueue->Enqueue(m_Storage, m_StorageAllocation);
    }

    [[nodiscard]] static auto
    Create(const VulkanResourceContext& Context, vk::AccelerationStructureTypeKHR Type, Uint64 Size, StringView Name)
        -> std::expected<UPtr<VulkanNativeAccelerationStructure>, ErrorMessage> {
        auto Result             = std::make_unique<VulkanNativeAccelerationStructure>();
        Result->m_DeferredQueue = &Context.GetDeferredDeletionQueue();

        const vk::BufferCreateInfo StorageCI{
            .size        = Size,
            .usage       = vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR,
            .sharingMode = vk::SharingMode::eExclusive,
        };
        const VmaAllocationCreateInfo AllocInfo{
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        };
        if (vmaCreateBuffer(Context.GetAllocator(),
                            reinterpret_cast<const VkBufferCreateInfo*>(&StorageCI),
                            &AllocInfo,
                            reinterpret_cast<VkBuffer*>(&Result->m_Storage),
                            &Result->m_StorageAllocation,
                            nullptr) != VK_SUCCESS)
            return std::unexpected(ErrorMessage("Failed to create acceleration structure storage via VMA"));
        Context.GetDebugUtils().SetObjectName(Result->m_Storage, Format("{}#Storage", Name));

        vk::AccelerationStructureCreateInfoKHR AccelerationStructureCI{
            .buffer = Result->m_Storage,
            .size   = Size,
            .type   = Type,
        };
        auto [CreateResult, RHIAccelerationStructure] =
            Context.GetDevice().createAccelerationStructureKHR(AccelerationStructureCI, nullptr);
        if (CreateResult != vk::Result::eSuccess) {
            // The storage buffer retires through the deferred queue when the failed Result payload dies — it
            // was never GPU-visible.
            return std::unexpected(ErrorMessage(
                Format("Failed to create Vulkan acceleration structure: {}", vk::to_string(CreateResult))));
        }

        Context.GetDebugUtils().SetObjectName(*RHIAccelerationStructure, Name);
        Result->m_AccelerationStructure = std::move(RHIAccelerationStructure);
        // The device address is immutable for the AS's lifetime; query it once here instead of once per
        // referencing TLAS instance.
        Result->m_Address               = Context.GetDevice().getAccelerationStructureAddressKHR(
            vk::AccelerationStructureDeviceAddressInfoKHR{.accelerationStructure = Result->Get()});
        return Result;
    }

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
        -> std::expected<UPtr<RHIBottomLevelAccelerationStructure>, ErrorMessage> {
        if (!VulkanCapability::Get().IsRayTracingAvailable())
            return std::unexpected(ErrorMessage("Cannot create BLAS because hardware ray tracing is unavailable"));

        auto* VertexBufferPtr = Desc.VertexBufferRef.TryGet();
        auto* IndexBufferPtr  = Desc.IndexBufferRef.TryGet();
        if (!VertexBufferPtr || !IndexBufferPtr) {
            return std::unexpected(ErrorMessage("BLAS triangle geometry requires both vertex and index buffers"));
        }
        // Geometry metadata is caller-carried mesh data; the buffers are unified identities and only supply
        // device addresses.
        if (Desc.VertexCount == 0 || Desc.IndexCount == 0 || Desc.IndexCount % 3 != 0) {
            return std::unexpected(ErrorMessage("BLAS triangle geometry requires non-empty triangle indices"));
        }
        if (Desc.VertexCount > std::numeric_limits<Uint32>::max() ||
            Desc.IndexCount / 3 > std::numeric_limits<Uint32>::max()) {
            return std::unexpected(ErrorMessage("BLAS geometry count exceeds Vulkan Uint32 limits"));
        }
        const vk::DeviceAddress VertexAddress = VertexBufferPtr->GetDeviceAddress();
        const vk::DeviceAddress IndexAddress  = IndexBufferPtr->GetDeviceAddress();
        if (VertexAddress == 0 || IndexAddress == 0)
            return std::unexpected(ErrorMessage("BLAS geometry buffer has no device address"));

        auto Result = std::make_unique<VulkanBottomLevelAccelerationStructure>(String(Name));

        Result->m_Geometry = vk::AccelerationStructureGeometryKHR{
            .geometryType = vk::GeometryTypeKHR::eTriangles,
            .geometry =
                vk::AccelerationStructureGeometryDataKHR{
                    .triangles =
                        vk::AccelerationStructureGeometryTrianglesDataKHR{
                            .vertexFormat  = vk::Format::eR32G32B32Sfloat,
                            .vertexData    = vk::DeviceOrHostAddressConstKHR{.deviceAddress = VertexAddress},
                            .vertexStride  = Desc.VertexStride,
                            .maxVertex     = static_cast<Uint32>(Desc.VertexCount - 1),
                            .indexType     = vk::IndexType::eUint32,
                            .indexData     = vk::DeviceOrHostAddressConstKHR{.deviceAddress = IndexAddress},
                            .transformData = vk::DeviceOrHostAddressConstKHR{.deviceAddress = 0},
                        }},
            .flags = vk::GeometryFlagBitsKHR::eOpaque,
        };
        const Uint32 PrimitiveCount = static_cast<Uint32>(Desc.IndexCount / 3);
        Result->m_Range             = vk::AccelerationStructureBuildRangeInfoKHR{
            .primitiveCount  = PrimitiveCount,
            .primitiveOffset = 0,
            .firstVertex     = 0,
            .transformOffset = 0,
        };

        const vk::AccelerationStructureBuildGeometryInfoKHR SizeQueryCI{
            .type          = vk::AccelerationStructureTypeKHR::eBottomLevel,
            .flags         = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
            .mode          = vk::BuildAccelerationStructureModeKHR::eBuild,
            .geometryCount = 1,
            .pGeometries   = &Result->m_Geometry,
        };
        const std::array<Uint32, 1> PrimitiveCounts{PrimitiveCount};
        const auto                  Sizes = Context.GetDevice().getAccelerationStructureBuildSizesKHR(
            vk::AccelerationStructureBuildTypeKHR::eDevice, SizeQueryCI, PrimitiveCounts);
        auto Native = VulkanNativeAccelerationStructure::Create(
            Context, vk::AccelerationStructureTypeKHR::eBottomLevel, Sizes.accelerationStructureSize, Name);
        if (!Native)
            return std::unexpected(Native.error().Append("Failed to allocate BLAS storage"));
        Result->m_Native      = std::move(*Native);
        Result->m_ScratchSize = Sizes.buildScratchSize;
        return UPtr<RHIBottomLevelAccelerationStructure>{std::move(Result)};
    }

    /// @brief Bake this BLAS's build entry for the manager's pending batch; the ref fills retention and the
    /// recorded-flag guard.
    [[nodiscard]] auto BakeBuildEntry(RHIRef<RHIBottomLevelAccelerationStructure> Self) const -> VulkanBlasBuildEntry {
        return VulkanBlasBuildEntry{
            .Blas        = std::move(Self),
            .Dst         = m_Native->Get(),
            .Geometry    = m_Geometry,
            .Range       = m_Range,
            .ScratchSize = m_ScratchSize,
        };
    }

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
        -> std::expected<UPtr<RHITopLevelAccelerationStructure>, ErrorMessage> {
        if (!VulkanCapability::Get().IsRayTracingAvailable())
            return std::unexpected(ErrorMessage("Cannot create TLAS because hardware ray tracing is unavailable"));

        auto Result = std::make_unique<VulkanTopLevelAccelerationStructure>(String(Name));

        // step 1: pack the instances. Slot order is the canonical instance order (InstanceIndex() mapping);
        // mask/offset/flags stay at their fixed defaults until a feature needs otherwise. BLAS device
        // addresses are immutable once their payloads are created, so packing is safe on the calling thread.
        Result->m_Instances.reserve(Desc.Instances.size());
        Result->m_BlasRefs.reserve(Desc.Instances.size());
        for (const auto& Instance : Desc.Instances) {
            auto* BottomLevelPtr = Instance.Blas.TryGet();
            if (!BottomLevelPtr)
                return std::unexpected(ErrorMessage(Format("TLAS '{}' instance is missing a BLAS", Name)));
            const auto& Blas = static_cast<const VulkanBottomLevelAccelerationStructure&>(*BottomLevelPtr);
            vk::AccelerationStructureInstanceKHR VkInstance{};
            VkInstance.setTransform(ToVkInstanceTransform(Instance.Transform))
                .setInstanceCustomIndex(0)
                .setMask(0xFF)
                .setInstanceShaderBindingTableRecordOffset(0)
                .setFlags(vk::GeometryInstanceFlagsKHR{})
                .setAccelerationStructureReference(Blas.GetAccelerationStructureAddress());
            Result->m_Instances.push_back(VkInstance);
            Result->m_BlasRefs.push_back(Instance.Blas);
        }

        // step 2: query the build sizes (instance count only) and materialize the storage.
        vk::AccelerationStructureGeometryInstancesDataKHR SizingData{
            .arrayOfPointers = vk::False,
            .data            = vk::DeviceOrHostAddressConstKHR{.deviceAddress = 0},
        };
        vk::AccelerationStructureGeometryKHR SizingGeometry{
            .geometryType = vk::GeometryTypeKHR::eInstances,
            .geometry     = vk::AccelerationStructureGeometryDataKHR{.instances = SizingData},
        };
        vk::AccelerationStructureBuildGeometryInfoKHR SizingCI{
            .type          = vk::AccelerationStructureTypeKHR::eTopLevel,
            .flags         = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
            .mode          = vk::BuildAccelerationStructureModeKHR::eBuild,
            .geometryCount = 1,
            .pGeometries   = &SizingGeometry,
        };
        const std::array<Uint32, 1> PrimitiveCounts{static_cast<Uint32>(Result->m_Instances.size())};
        const auto                  Sizes = Context.GetDevice().getAccelerationStructureBuildSizesKHR(
            vk::AccelerationStructureBuildTypeKHR::eDevice, SizingCI, PrimitiveCounts);
        auto Native = VulkanNativeAccelerationStructure::Create(
            Context, vk::AccelerationStructureTypeKHR::eTopLevel, Sizes.accelerationStructureSize, Name);
        if (!Native)
            return std::unexpected(Native.error().Append("Failed to allocate TLAS storage"));
        Result->m_Native      = std::move(*Native);
        Result->m_ScratchSize = Sizes.buildScratchSize;
        return UPtr<RHITopLevelAccelerationStructure>{std::move(Result)};
    }

    /// @brief Bake this TLAS's build entry for the manager's pending batch; the ref fills retention.
    [[nodiscard]] auto BakeBuildEntry(RHIRef<RHITopLevelAccelerationStructure> Self) const -> VulkanTlasBuildEntry {
        return VulkanTlasBuildEntry{
            .Tlas        = std::move(Self),
            .Dst         = m_Native->Get(),
            .Instances   = m_Instances,
            .BlasRefs    = m_BlasRefs,
            .ScratchSize = m_ScratchSize,
        };
    }

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
        -> std::expected<VulkanAccelerationStructureManager, ErrorMessage> {
        VulkanAccelerationStructureManager Manager;
        Manager.m_Context = &Context;
        if (!VulkanCapability::Get().IsRayTracingAvailable())
            return Manager;
        const Uint64 ScratchAlignment =
            std::max<Uint64>(VulkanCapability::Get()
                                 .GetProperties<vk::PhysicalDeviceAccelerationStructurePropertiesKHR>()
                                 .minAccelerationStructureScratchOffsetAlignment,
                             1);
        auto ScratchRing = VulkanRingBuffer::Create(Context,
                                                    "Internal/Buffer/ASScratchRing",
                                                    vk::BufferUsageFlagBits::eStorageBuffer |
                                                        vk::BufferUsageFlagBits::eShaderDeviceAddress,
                                                    ScratchRingCapacity,
                                                    ScratchAlignment,
                                                    VulkanRingMemoryClass::DeviceLocal);
        if (!ScratchRing)
            return std::unexpected(
                ScratchRing.error().Append("VulkanAccelerationStructureManager: scratch ring creation failed"));
        Manager.m_ScratchRing = std::move(*ScratchRing);
        auto InstanceRing =
            VulkanRingBuffer::Create(Context,
                                     "Internal/Buffer/ASInstanceRing",
                                     vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR |
                                         vk::BufferUsageFlagBits::eShaderDeviceAddress,
                                     InstanceRingCapacity,
                                     kTlasInstanceDataAlignment);
        if (!InstanceRing)
            return std::unexpected(
                InstanceRing.error().Append("VulkanAccelerationStructureManager: instance ring creation failed"));
        Manager.m_InstanceRing        = std::move(*InstanceRing);
        Manager.m_IsRayTracingEnabled = true;
        return Manager;
    }

    /// @brief Release the VMA-backed rings while the allocator is still alive.
    ///
    /// Device shutdown ordering requires this to run before the allocator is destroyed.
    auto Shutdown() -> void {
        m_ScratchRing         = {};
        m_InstanceRing        = {};
        m_Context             = nullptr;
        m_IsRayTracingEnabled = false;
    }

    /// @brief Create and materialize a BLAS synchronously on the calling thread: validation, geometry
    /// baking, the build-size query, and native storage/handle/address allocation are all thread-safe. The
    /// ref publishes Ready at return; the build rides BuildBlas.
    /// @param Name  logical RHI name; becomes the debug-utils object name.
    /// @param Desc  BLAS description; the referenced geometry buffers must be Ready.
    [[nodiscard]] auto CreateBlas(StringView Name, const RHIBottomLevelAccelerationStructureDesc& Desc)
        -> std::expected<RHIRef<RHIBottomLevelAccelerationStructure>, ErrorMessage> {
        auto Resource = RHIRef<RHIBottomLevelAccelerationStructure>::Create();
        auto Result   = VulkanBottomLevelAccelerationStructure::Create(*m_Context, Name, Desc);
        if (!Result) {
            Resource.MarkFailed(Result.error());
            return std::unexpected(Result.error());
        }
        if (auto Publish = Resource.Publish(std::move(*Result), RHIRefState::Ready); !Publish)
            return std::unexpected(Publish.error());
        return Resource;
    }

    /// @brief Create and materialize the per-frame TLAS synchronously on the calling thread: instance
    /// packing, the build-size query, and native storage allocation are all thread-safe. The ref publishes
    /// Ready at return; the build rides BuildTlas.
    /// @param Name  logical RHI name; becomes the debug-utils object name.
    /// @param Desc  TLAS description; every instance must reference a published BLAS payload.
    [[nodiscard]] auto CreateTlas(StringView Name, const RHITopLevelAccelerationStructureDesc& Desc)
        -> std::expected<RHIRef<RHITopLevelAccelerationStructure>, ErrorMessage> {
        auto Resource = RHIRef<RHITopLevelAccelerationStructure>::Create();
        auto Result   = VulkanTopLevelAccelerationStructure::Create(*m_Context, Name, Desc);
        if (!Result) {
            Resource.MarkFailed(Result.error());
            return std::unexpected(Result.error());
        }
        if (auto Publish = Resource.Publish(std::move(*Result), RHIRefState::Ready); !Publish)
            return std::unexpected(Publish.error());
        return Resource;
    }

    /// @brief Build a BLAS: hand a frame-affined task carrying the ref to the RHI thread.
    ///
    /// The task (drained at the matching RHI frame, before Execute) bakes the build entry from the payload
    /// and appends it to the pending list; Execute records the whole batch at frame start, after the upload
    /// batch, so same-frame consumers are safe by submission order. Frame affinity binds the build to the
    /// producer frame; a stale task (its frame never executed) is discarded by the task graph.
    /// @param Blas  BLAS ref; must be Ready at the call site.
    [[nodiscard]] auto BuildBlas(RHIRef<RHIBottomLevelAccelerationStructure> Blas)
        -> std::expected<void, ErrorMessage> {
        if (!Blas.TryGet())
            return std::unexpected(ErrorMessage("BuildBlas: BLAS ref is not Ready"));
        auto EnqueueResult =
            TaskGraph::Get().EnqueueFrameTask(ThreadQueue::RHI, [this, Blas = std::move(Blas)]() mutable {
                auto* Payload = Blas.TryGet();
                if (!Payload) {
                    LogWarning("BuildBlas: BLAS ref is not Ready when the build task ran");
                    return;
                }
                m_PendingBlasBuilds.push_back(
                    static_cast<const VulkanBottomLevelAccelerationStructure&>(*Payload).BakeBuildEntry(
                        std::move(Blas)));
            });
        if (!EnqueueResult)
            return std::unexpected(EnqueueResult.error().Append("BuildBlas: frame task enqueue failed"));
        return {};
    }

    /// @brief Build a per-frame TLAS: hand a frame-affined task carrying the ref to the RHI thread. Same
    /// contract as BuildBlas; the build is recorded after the BLAS batch, ahead of every pass.
    /// @param Tlas  TLAS ref; must be Ready at the call site.
    [[nodiscard]] auto BuildTlas(RHIRef<RHITopLevelAccelerationStructure> Tlas) -> std::expected<void, ErrorMessage> {
        if (!Tlas.TryGet())
            return std::unexpected(ErrorMessage("BuildTlas: TLAS ref is not Ready"));
        auto EnqueueResult =
            TaskGraph::Get().EnqueueFrameTask(ThreadQueue::RHI, [this, Tlas = std::move(Tlas)]() mutable {
                auto* Payload = Tlas.TryGet();
                if (!Payload) {
                    LogWarning("BuildTlas: TLAS ref is not Ready when the build task ran");
                    return;
                }
                m_PendingTlasBuilds.push_back(
                    static_cast<const VulkanTopLevelAccelerationStructure&>(*Payload).BakeBuildEntry(std::move(Tlas)));
            });
        if (!EnqueueResult)
            return std::unexpected(EnqueueResult.error().Append("BuildTlas: frame task enqueue failed"));
        return {};
    }

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
        -> std::expected<void, ErrorMessage> {
        if (m_PendingBlasBuilds.empty() && m_PendingTlasBuilds.empty())
            return {};
        if (!m_IsRayTracingEnabled) {
            LogWarning("AS build batch: ray tracing is unavailable, skipping {} BLAS / {} TLAS builds",
                       m_PendingBlasBuilds.size(),
                       m_PendingTlasBuilds.size());
            m_PendingBlasBuilds.clear();
            m_PendingTlasBuilds.clear();
            return {};
        }
        auto Blases = std::exchange(m_PendingBlasBuilds, {});
        auto Tlases = std::exchange(m_PendingTlasBuilds, {});

        vk::CommandBufferAllocateInfo Alloc{
            .commandPool        = *FC.SubPool,
            .level              = vk::CommandBufferLevel::eSecondary,
            .commandBufferCount = 1,
        };
        auto AllocResult = m_Context->GetDevice().allocateCommandBuffers(Alloc);
        if (AllocResult.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("AS build batch: failed to allocate the secondary CB"));
        m_Context->GetDebugUtils().SetObjectName(
            *AllocResult.value[0], Format("Internal/CommandBuffer/Secondary/Frame{}/ASBuild", FrameIndex));
        auto& SecBuf = FC.ScratchSecondaries.emplace_back(std::move(AllocResult.value[0]));

        vk::CommandBufferInheritanceInfo Inheritance{};
        vk::CommandBufferBeginInfo       BeginCI{
            .flags            = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
            .pInheritanceInfo = &Inheritance,
        };
        if (auto R = SecBuf.begin(BeginCI); R != vk::Result::eSuccess)
            return std::unexpected(
                ErrorMessage(Format("AS build batch: secondary CB begin failed: {}", vk::to_string(R))));

        // step 1: the BLAS lane — ONE batched build call for the whole lane, sealed by a single build-write
        // to build-read barrier so the TLAS lane can consume the results.
        if (!Blases.empty()) {
            const LabelScope BlasLabel{*m_Context, SecBuf, "BLASBatch"};
            RecordBlasLane(SecBuf, Blases);
        }
        // step 2: the TLAS lane — each build sealed by a build-write to ray-tracing-shader-read barrier so
        // this frame's trace can consume the result.
        if (!Tlases.empty()) {
            const LabelScope TlasLabel{*m_Context, SecBuf, "TLASBuild"};
            RecordTlasLane(SecBuf, Tlases);
        }

        if (auto R = SecBuf.end(); R != vk::Result::eSuccess)
            return std::unexpected(
                ErrorMessage(Format("AS build batch: secondary CB end failed: {}", vk::to_string(R))));
        Primary.executeCommands({static_cast<vk::CommandBuffer>(*SecBuf)});
        return {};
    }

    /// @brief Tick-chain relay: forward the frame-begin timeline value to both rings.
    /// @param FrameBeginTimelineValue  host-reserved frame-begin value, issued by the device Tick.
    auto Tick(Uint64 FrameBeginTimelineValue) -> void {
        if (!m_IsRayTracingEnabled)
            return;
        m_ScratchRing.Tick(FrameBeginTimelineValue);
        m_InstanceRing.Tick(FrameBeginTimelineValue);
    }

  private:
    /// @brief BLAS lane of the frame-start batch: a scratch carve plus one batch entry per request, then ONE
    /// build call and the aggregated barrier. Fail-soft per entry (logged and skipped): the recorded flag
    /// guards double builds, and a ring allocation failure drops that entry alone.
    auto RecordBlasLane(vk::raii::CommandBuffer& CmdBuffer, std::span<VulkanBlasBuildEntry> Entries) -> void {
        std::vector<vk::AccelerationStructureBuildGeometryInfoKHR> Infos;
        std::vector<vk::AccelerationStructureBuildRangeInfoKHR>    Ranges;
        Infos.reserve(Entries.size());
        Ranges.reserve(Entries.size());
        for (auto& Entry : Entries) {
            auto* Payload = Entry.Blas.TryGet();
            if (!Payload) {
                LogWarning("BLAS batch: skipped an entry whose payload is gone");
                continue;
            }
            auto& Blas = static_cast<VulkanBottomLevelAccelerationStructure&>(*Payload);
            if (Blas.IsBuildRecorded()) {
                LogError("BLAS batch: '{}' build requested twice; a static BLAS is built exactly once", Blas.GetName());
                continue;
            }
            auto Scratch = m_ScratchRing.Allocate(Entry.ScratchSize);
            if (!Scratch) {
                LogWarning("BLAS batch: scratch ring allocation failed for '{}': {}",
                           Blas.GetName(),
                           Scratch.error().ToString());
                continue;
            }
            // pGeometries points into the drained Entries vector, which is finalized before this loop runs.
            Infos.push_back(vk::AccelerationStructureBuildGeometryInfoKHR{
                .type                     = vk::AccelerationStructureTypeKHR::eBottomLevel,
                .flags                    = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
                .mode                     = vk::BuildAccelerationStructureModeKHR::eBuild,
                .dstAccelerationStructure = Entry.Dst,
                .geometryCount            = 1,
                .pGeometries              = &Entry.Geometry,
                .scratchData              = vk::DeviceOrHostAddressKHR{.deviceAddress = Scratch->DeviceAddress},
            });
            Ranges.push_back(Entry.Range);
            Blas.MarkBuildRecorded();
        }
        if (Infos.empty())
            return;

        std::vector<const vk::AccelerationStructureBuildRangeInfoKHR*> RangePtrs;
        RangePtrs.reserve(Ranges.size());
        for (const auto& Range : Ranges)
            RangePtrs.push_back(&Range);
        CmdBuffer.buildAccelerationStructuresKHR(Infos, RangePtrs);

        const vk::MemoryBarrier2 BuildBarrier{
            .srcStageMask  = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
            .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
            .dstStageMask  = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
            .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR,
        };
        CmdBuffer.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &BuildBarrier});
    }

    /// @brief TLAS lane of the frame-start batch: per entry, validate the referenced BLAS builds (the
    /// recorded flag — the BLAS lane runs first, so a flag set this frame means submission order covers this
    /// build), carve scratch and instance ranges, copy the packed instances, build, and seal with a
    /// build-write to ray-tracing-shader-read barrier. Fail-soft per entry.
    auto RecordTlasLane(vk::raii::CommandBuffer& CmdBuffer, std::span<VulkanTlasBuildEntry> Entries) -> void {
        for (auto& Entry : Entries) {
            if (Entry.Instances.empty()) {
                LogWarning("TLAS batch: skipped a TLAS with no instances");
                continue;
            }
            bool AllRecorded = true;
            for (const auto& BlasRef : Entry.BlasRefs) {
                auto* BlasPtr = BlasRef.TryGet();
                if (!BlasPtr ||
                    !static_cast<const VulkanBottomLevelAccelerationStructure&>(*BlasPtr).IsBuildRecorded()) {
                    AllRecorded = false;
                    break;
                }
            }
            if (!AllRecorded) {
                LogError("TLAS batch: skipped a TLAS referencing a BLAS that has not been built");
                continue;
            }
            auto Scratch = m_ScratchRing.Allocate(Entry.ScratchSize);
            if (!Scratch) {
                LogWarning("TLAS batch: scratch ring allocation failed: {}", Scratch.error().ToString());
                continue;
            }
            const Uint64 InstanceBytes = Entry.Instances.size() * sizeof(vk::AccelerationStructureInstanceKHR);
            auto         Instances     = m_InstanceRing.Allocate(InstanceBytes);
            if (!Instances) {
                LogWarning("TLAS batch: instance ring allocation failed: {}", Instances.error().ToString());
                continue;
            }
            // The instance ring is contractually host-coherent, so the memcpy is implicitly visible to this
            // frame's submission — no HOST barrier.
            std::memcpy(Instances->MappedData, Entry.Instances.data(), InstanceBytes);

            vk::AccelerationStructureGeometryInstancesDataKHR BuildData{
                .arrayOfPointers = vk::False,
                .data            = vk::DeviceOrHostAddressConstKHR{.deviceAddress = Instances->DeviceAddress},
            };
            vk::AccelerationStructureGeometryKHR BuildGeometry{
                .geometryType = vk::GeometryTypeKHR::eInstances,
                .geometry     = vk::AccelerationStructureGeometryDataKHR{.instances = BuildData},
            };
            const vk::AccelerationStructureBuildGeometryInfoKHR BuildCI{
                .type                     = vk::AccelerationStructureTypeKHR::eTopLevel,
                .flags                    = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
                .mode                     = vk::BuildAccelerationStructureModeKHR::eBuild,
                .dstAccelerationStructure = Entry.Dst,
                .geometryCount            = 1,
                .pGeometries              = &BuildGeometry,
                .scratchData              = vk::DeviceOrHostAddressKHR{.deviceAddress = Scratch->DeviceAddress},
            };
            const vk::AccelerationStructureBuildRangeInfoKHR  Range{.primitiveCount =
                                                                        static_cast<Uint32>(Entry.Instances.size())};
            const vk::AccelerationStructureBuildRangeInfoKHR* RangePtr = &Range;
            CmdBuffer.buildAccelerationStructuresKHR(BuildCI, {RangePtr});

            const vk::MemoryBarrier2 BuildBarrier{
                .srcStageMask  = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
                .dstStageMask  = vk::PipelineStageFlagBits2::eRayTracingShaderKHR,
                .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR,
            };
            CmdBuffer.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &BuildBarrier});
        }
    }

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
