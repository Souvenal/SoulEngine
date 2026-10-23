module;

#include <hlsl++.h>
#include <vk_mem_alloc.h>

module RHIVulkan.AccelerationStructure;

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
[[nodiscard]] auto ToVkInstanceTransform(const hlslpp::float4x4& Matrix) -> vk::TransformMatrixKHR {
    return vk::TransformMatrixKHR{std::array<std::array<Float32, 4>, 3>{
        std::array<Float32, 4>{Matrix[0].x, Matrix[1].x, Matrix[2].x, Matrix[3].x},
        std::array<Float32, 4>{Matrix[0].y, Matrix[1].y, Matrix[2].y, Matrix[3].y},
        std::array<Float32, 4>{Matrix[0].z, Matrix[1].z, Matrix[2].z, Matrix[3].z},
    }};
}

VulkanNativeAccelerationStructure::~VulkanNativeAccelerationStructure() {
    if (m_StorageAllocation)
        m_DeferredQueue->Enqueue(m_Storage, m_StorageAllocation);
}

auto VulkanNativeAccelerationStructure::Create(const VulkanResourceContext&     Context,
                                               vk::AccelerationStructureTypeKHR Type,
                                               Uint64                           Size,
                                               StringView                       Name)
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
        return std::unexpected(
            ErrorMessage(Format("Failed to create Vulkan acceleration structure: {}", vk::to_string(CreateResult))));
    }

    Context.GetDebugUtils().SetObjectName(*RHIAccelerationStructure, Name);
    Result->m_AccelerationStructure = std::move(RHIAccelerationStructure);
    // The device address is immutable for the AS's lifetime; query it once here instead of once per
    // referencing TLAS instance.
    Result->m_Address               = Context.GetDevice().getAccelerationStructureAddressKHR(
        vk::AccelerationStructureDeviceAddressInfoKHR{.accelerationStructure = Result->Get()});
    return Result;
}

auto VulkanBottomLevelAccelerationStructure::Create(const VulkanResourceContext&                   Context,
                                                    StringView                                     Name,
                                                    const RHIBottomLevelAccelerationStructureDesc& Desc)
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

auto VulkanBottomLevelAccelerationStructure::BakeBuildEntry(RHIRef<RHIBottomLevelAccelerationStructure> Self) const
    -> VulkanBlasBuildEntry {
    return VulkanBlasBuildEntry{
        .Blas        = std::move(Self),
        .Dst         = m_Native->Get(),
        .Geometry    = m_Geometry,
        .Range       = m_Range,
        .ScratchSize = m_ScratchSize,
    };
}

auto VulkanTopLevelAccelerationStructure::Create(const VulkanResourceContext&                Context,
                                                 StringView                                  Name,
                                                 const RHITopLevelAccelerationStructureDesc& Desc)
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

auto VulkanTopLevelAccelerationStructure::BakeBuildEntry(RHIRef<RHITopLevelAccelerationStructure> Self) const
    -> VulkanTlasBuildEntry {
    return VulkanTlasBuildEntry{
        .Tlas        = std::move(Self),
        .Dst         = m_Native->Get(),
        .Instances   = m_Instances,
        .BlasRefs    = m_BlasRefs,
        .ScratchSize = m_ScratchSize,
    };
}

auto VulkanAccelerationStructureManager::Create(const VulkanResourceContext& Context,
                                                Uint64                       ScratchRingCapacity,
                                                Uint64                       InstanceRingCapacity)
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
    auto InstanceRing = VulkanRingBuffer::Create(Context,
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

auto VulkanAccelerationStructureManager::Shutdown() -> void {
    m_ScratchRing         = {};
    m_InstanceRing        = {};
    m_Context             = nullptr;
    m_IsRayTracingEnabled = false;
}

auto VulkanAccelerationStructureManager::CreateBlas(StringView                                     Name,
                                                    const RHIBottomLevelAccelerationStructureDesc& Desc)
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

auto VulkanAccelerationStructureManager::CreateTlas(StringView Name, const RHITopLevelAccelerationStructureDesc& Desc)
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

auto VulkanAccelerationStructureManager::BuildBlas(RHIRef<RHIBottomLevelAccelerationStructure> Blas)
    -> std::expected<void, ErrorMessage> {
    if (!Blas.TryGet())
        return std::unexpected(ErrorMessage("BuildBlas: BLAS ref is not Ready"));
    auto EnqueueResult = TaskGraph::Get().EnqueueFrameTask(ThreadQueue::RHI, [this, Blas = std::move(Blas)]() mutable {
        auto* Payload = Blas.TryGet();
        if (!Payload) {
            LogWarning("BuildBlas: BLAS ref is not Ready when the build task ran");
            return;
        }
        m_PendingBlasBuilds.push_back(
            static_cast<const VulkanBottomLevelAccelerationStructure&>(*Payload).BakeBuildEntry(std::move(Blas)));
    });
    if (!EnqueueResult)
        return std::unexpected(EnqueueResult.error().Append("BuildBlas: frame task enqueue failed"));
    return {};
}

auto VulkanAccelerationStructureManager::BuildTlas(RHIRef<RHITopLevelAccelerationStructure> Tlas)
    -> std::expected<void, ErrorMessage> {
    if (!Tlas.TryGet())
        return std::unexpected(ErrorMessage("BuildTlas: TLAS ref is not Ready"));
    auto EnqueueResult = TaskGraph::Get().EnqueueFrameTask(ThreadQueue::RHI, [this, Tlas = std::move(Tlas)]() mutable {
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

auto VulkanAccelerationStructureManager::RecordBuildBatch(VulkanFrameContext&      FC,
                                                          Uint32                   FrameIndex,
                                                          vk::raii::CommandBuffer& Primary)
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
    m_Context->GetDebugUtils().SetObjectName(*AllocResult.value[0],
                                             Format("Internal/CommandBuffer/Secondary/Frame{}/ASBuild", FrameIndex));
    auto& SecBuf = FC.ScratchSecondaries.emplace_back(std::move(AllocResult.value[0]));

    vk::CommandBufferInheritanceInfo Inheritance{};
    vk::CommandBufferBeginInfo       BeginCI{
        .flags            = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
        .pInheritanceInfo = &Inheritance,
    };
    if (auto R = SecBuf.begin(BeginCI); R != vk::Result::eSuccess)
        return std::unexpected(ErrorMessage(Format("AS build batch: secondary CB begin failed: {}", vk::to_string(R))));

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
        return std::unexpected(ErrorMessage(Format("AS build batch: secondary CB end failed: {}", vk::to_string(R))));
    Primary.executeCommands({static_cast<vk::CommandBuffer>(*SecBuf)});
    return {};
}

auto VulkanAccelerationStructureManager::Tick(Uint64 FrameBeginTimelineValue) -> void {
    if (!m_IsRayTracingEnabled)
        return;
    m_ScratchRing.Tick(FrameBeginTimelineValue);
    m_InstanceRing.Tick(FrameBeginTimelineValue);
}

auto VulkanAccelerationStructureManager::RecordBlasLane(vk::raii::CommandBuffer&        CmdBuffer,
                                                        std::span<VulkanBlasBuildEntry> Entries) -> void {
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
            LogWarning(
                "BLAS batch: scratch ring allocation failed for '{}': {}", Blas.GetName(), Scratch.error().ToString());
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

auto VulkanAccelerationStructureManager::RecordTlasLane(vk::raii::CommandBuffer&        CmdBuffer,
                                                        std::span<VulkanTlasBuildEntry> Entries) -> void {
    for (auto& Entry : Entries) {
        if (Entry.Instances.empty()) {
            LogWarning("TLAS batch: skipped a TLAS with no instances");
            continue;
        }
        bool AllRecorded = true;
        for (const auto& BlasRef : Entry.BlasRefs) {
            auto* BlasPtr = BlasRef.TryGet();
            if (!BlasPtr || !static_cast<const VulkanBottomLevelAccelerationStructure&>(*BlasPtr).IsBuildRecorded()) {
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

} // namespace SoulEngine
