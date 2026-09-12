module;

#include <hlsl++.h>
#include <vk_mem_alloc.h>

export module Vulkan:AccelerationStructure;

import Core;
import RHI;
import vulkan;
import std;

import :Buffer;
import :Capability;
import :Context;
import :Debug;

namespace SoulEngine {

namespace {

/// Pack a row-vector hlslpp world transform into Vulkan's row-major 3x4:
/// the upper-left 3x3 block is transposed, the fourth row becomes translation.
[[nodiscard]] auto ToVkInstanceTransform(const hlslpp::float4x4& Matrix) -> vk::TransformMatrixKHR {
    return vk::TransformMatrixKHR{std::array<std::array<Float32, 4>, 3>{
        std::array<Float32, 4>{Matrix[0].x, Matrix[1].x, Matrix[2].x, Matrix[3].x},
        std::array<Float32, 4>{Matrix[0].y, Matrix[1].y, Matrix[2].y, Matrix[3].y},
        std::array<Float32, 4>{Matrix[0].z, Matrix[1].z, Matrix[2].z, Matrix[3].z},
    }};
}

class VulkanNativeAccelerationStructure final {
  public:
    VulkanNativeAccelerationStructure(VulkanDeviceBuffer                 Storage,
                                      vk::raii::AccelerationStructureKHR AccelerationStructure)
        : m_Storage(std::move(Storage)), m_AccelerationStructure(std::move(AccelerationStructure)) {}

    [[nodiscard]] static auto
    Create(const VulkanResourceContext& Context, vk::AccelerationStructureTypeKHR Type, Uint64 Size, StringView Name)
        -> std::expected<SPtr<VulkanNativeAccelerationStructure>, ErrorMessage> {
        auto StorageResult = VulkanDeviceBuffer::Create(
            Context, Format("{}#Storage", Name), Size, vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR);
        if (!StorageResult)
            return std::unexpected(StorageResult.error());
        auto Storage = std::move(*StorageResult);

        vk::AccelerationStructureCreateInfoKHR AccelerationStructureCI{
            .buffer = Storage.Get(),
            .size   = Size,
            .type   = Type,
        };
        auto [Result, RHIAccelerationStructure] =
            Context.GetDevice().createAccelerationStructureKHR(AccelerationStructureCI, nullptr);
        if (Result != vk::Result::eSuccess) {
            return std::unexpected(
                ErrorMessage(Format("Failed to create Vulkan acceleration structure: {}", vk::to_string(Result))));
        }

        Context.GetDebugUtils().SetObjectName(*RHIAccelerationStructure, Name);
        return std::make_shared<VulkanNativeAccelerationStructure>(std::move(Storage),
                                                                   std::move(RHIAccelerationStructure));
    }

    [[nodiscard]] auto Get() const -> vk::AccelerationStructureKHR {
        return *m_AccelerationStructure;
    }

  private:
    VulkanDeviceBuffer                 m_Storage;
    vk::raii::AccelerationStructureKHR m_AccelerationStructure;
};

[[nodiscard]] auto GetAccelerationStructureAddress(vk::raii::Device& Device, vk::AccelerationStructureKHR Structure)
    -> vk::DeviceAddress {
    return Device.getAccelerationStructureAddressKHR(
        vk::AccelerationStructureDeviceAddressInfoKHR{.accelerationStructure = Structure});
}

} // namespace

/// One materialized BLAS build entry for the frame-start batch. BuildInfo
/// references the payload's retained geometry; the caller splits all entries
/// into one contiguous info array plus range pointers for a single
/// buildAccelerationStructuresKHR call and retains the shared scratch until
/// the submission completes.
struct BlasBuildBatchEntry {
    vk::AccelerationStructureBuildGeometryInfoKHR BuildInfo = {};
    vk::AccelerationStructureBuildRangeInfoKHR    RangeInfo = {};
};

/// Vulkan GPU payload for reusable object-space triangle geometry.
///
/// Hollow until built: Create validates the inputs and bakes the build
/// description (device addresses are fixed because the caller guarantees the
/// buffers are Ready) but allocates no GPU resources. The frame-start batch
/// (Build) queries sizes via GetAccelerationStructureBuildSizes, carves an
/// aligned offset out of the batch's one shared scratch buffer, and
/// materializes storage plus the batch entry via MaterializeBuildInfos. A
/// static BLAS is built exactly once.
class VulkanBottomLevelAccelerationStructure final : public RHIBottomLevelAccelerationStructure {
  public:
    explicit VulkanBottomLevelAccelerationStructure(String Name)
        : RHIBottomLevelAccelerationStructure(std::move(Name)) {}

    ~VulkanBottomLevelAccelerationStructure() override = default;

    VulkanBottomLevelAccelerationStructure(const VulkanBottomLevelAccelerationStructure&)                    = delete;
    auto operator=(const VulkanBottomLevelAccelerationStructure&) -> VulkanBottomLevelAccelerationStructure& = delete;

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
        auto& VertexBuf = static_cast<const VulkanVertexBuffer&>(*VertexBufferPtr);
        auto& IndexBuf  = static_cast<const VulkanIndexBuffer&>(*IndexBufferPtr);
        if (VertexBuf.GetVertexCount() == 0 || IndexBuf.GetIndexCount() == 0 || IndexBuf.GetIndexCount() % 3 != 0) {
            return std::unexpected(ErrorMessage("BLAS triangle geometry requires non-empty triangle indices"));
        }
        if (VertexBuf.GetVertexCount() > std::numeric_limits<Uint32>::max() ||
            IndexBuf.GetIndexCount() / 3 > std::numeric_limits<Uint32>::max()) {
            return std::unexpected(ErrorMessage("BLAS geometry count exceeds Vulkan Uint32 limits"));
        }
        const vk::DeviceAddress VertexAddress = VertexBuf.GetDeviceAddress();
        const vk::DeviceAddress IndexAddress  = IndexBuf.GetDeviceAddress();
        if (VertexAddress == 0 || IndexAddress == 0)
            return std::unexpected(ErrorMessage("BLAS geometry buffer has no device address"));

        auto Result = std::make_unique<VulkanBottomLevelAccelerationStructure>(String(Name));
        Result->m_Context = &Context;

        vk::AccelerationStructureGeometryTrianglesDataKHR Triangles{
            .vertexFormat  = vk::Format::eR32G32B32Sfloat,
            .vertexData    = vk::DeviceOrHostAddressConstKHR{.deviceAddress = VertexAddress},
            .vertexStride  = VertexBuf.GetStride(),
            .maxVertex     = static_cast<Uint32>(VertexBuf.GetVertexCount() - 1),
            .indexType     = vk::IndexType::eUint32,
            .indexData     = vk::DeviceOrHostAddressConstKHR{.deviceAddress = IndexAddress},
            .transformData = vk::DeviceOrHostAddressConstKHR{.deviceAddress = 0},
        };
        Result->m_Geometry = vk::AccelerationStructureGeometryKHR{
            .geometryType = vk::GeometryTypeKHR::eTriangles,
            .geometry     = vk::AccelerationStructureGeometryDataKHR{.triangles = Triangles},
            .flags        = vk::GeometryFlagBitsKHR::eOpaque,
        };
        const Uint32 PrimitiveCount = static_cast<Uint32>(IndexBuf.GetIndexCount() / 3);
        Result->m_PrimitiveCount = PrimitiveCount;
        Result->m_Range          = vk::AccelerationStructureBuildRangeInfoKHR{
            .primitiveCount  = PrimitiveCount,
            .primitiveOffset = 0,
            .firstVertex     = 0,
            .transformOffset = 0,
        };
        return UPtr<RHIBottomLevelAccelerationStructure>{std::move(Result)};
    }

    /// Query this BLAS's build sizes through the device. The frame-start batch
    /// sums buildScratchSize across the batch before allocating the shared
    /// scratch, then hands accelerationStructureSize back to
    /// MaterializeBuildInfos.
    [[nodiscard]] auto GetAccelerationStructureBuildSizes() const -> vk::AccelerationStructureBuildSizesInfoKHR {
        const vk::AccelerationStructureBuildGeometryInfoKHR SizeQueryCI{
            .type          = vk::AccelerationStructureTypeKHR::eBottomLevel,
            .flags         = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
            .mode          = vk::BuildAccelerationStructureModeKHR::eBuild,
            .geometryCount = 1,
            .pGeometries   = &m_Geometry,
        };
        const std::array<Uint32, 1> PrimitiveCounts{m_PrimitiveCount};
        return m_Context->GetDevice().getAccelerationStructureBuildSizesKHR(
            vk::AccelerationStructureBuildTypeKHR::eDevice, SizeQueryCI, PrimitiveCounts);
    }

    /// Materialize this BLAS's native storage and produce its batch entry.
    /// Called once by the frame-start batch (Build); a static BLAS is built
    /// exactly once. ScratchAddressOffset is the aligned offset the caller
    /// carved out of the batch's shared scratch buffer.
    [[nodiscard]] auto MaterializeBuildInfos(const VulkanResourceContext& Context,
                                             vk::DeviceSize               AccelerationStructureSize,
                                             const VulkanDeviceBuffer&    Scratch,
                                             vk::DeviceSize               ScratchAddressOffset)
        -> std::expected<BlasBuildBatchEntry, ErrorMessage> {
        if (m_Built)
            return std::unexpected(ErrorMessage("BLAS build requested twice; a static BLAS is built exactly once"));
        // Trusted upstream constraints (GeometryUploader::UploadBlas gate plus
        // hollow Create validation): exactly one non-empty triangles geometry
        // with baked, non-zero device addresses, and Context is this payload's
        // own resource context. Violations are rejected at descriptor
        // creation, so the batch records this payload without re-validating.
        auto Native = VulkanNativeAccelerationStructure::Create(
            Context, vk::AccelerationStructureTypeKHR::eBottomLevel, AccelerationStructureSize, GetName());
        if (!Native)
            return std::unexpected(Native.error().Append("Failed to allocate BLAS storage"));
        m_Native = std::move(*Native);
        m_Built  = true;
        return BlasBuildBatchEntry{
            .BuildInfo =
                vk::AccelerationStructureBuildGeometryInfoKHR{
                    .type                     = vk::AccelerationStructureTypeKHR::eBottomLevel,
                    .flags                    = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
                    .mode                     = vk::BuildAccelerationStructureModeKHR::eBuild,
                    .dstAccelerationStructure = m_Native->Get(),
                    .geometryCount            = 1,
                    .pGeometries              = &m_Geometry,
                    .scratchData =
                        vk::DeviceOrHostAddressKHR{
                            .deviceAddress = Scratch.GetDeviceAddress() + ScratchAddressOffset},
                },
            .RangeInfo = m_Range,
        };
    }

    /// Frame-start batch build: materialize every pending hollow BLAS and
    /// record ONE buildAccelerationStructuresKHR for the whole batch, then a
    /// single build-write to build-read barrier so later work in the same
    /// command buffer (the TLAS build) can consume the results. All BLASs
    /// share ONE scratch buffer carved into aligned offsets
    /// (minAccelerationStructureScratchOffsetAlignment); the caller retains
    /// the returned scratch until the submission completes. Returns nullptr
    /// when there was nothing to build.
    ///
    /// Failure semantics: a per-BLAS problem is logged and skipped — that BLAS
    /// is never rebuilt, and the upstream guarantees an enqueued descriptor is
    /// valid, so skipping only guards genuine surprises. A batch-structural
    /// failure (shared scratch allocation) is returned as an error.
    [[nodiscard]] static auto
    Build(const VulkanResourceContext&                                 Context,
          vk::raii::CommandBuffer&                                     CmdBuffer,
          std::span<const RHIRef<RHIBottomLevelAccelerationStructure>> Blases,
          StringView                                                   ScratchName)
        -> std::expected<SPtr<VulkanDeviceBuffer>, ErrorMessage> {
        if (Blases.empty())
            return nullptr;

        struct Candidate {
            VulkanBottomLevelAccelerationStructure*    Blas = nullptr;
            vk::AccelerationStructureBuildSizesInfoKHR Sizes{};
        };
        std::vector<Candidate> Candidates;
        Candidates.reserve(Blases.size());
        Uint64 TotalScratchSize = 0;
        for (const auto& BlasRef : Blases) {
            auto* BlasPtr = BlasRef.TryGet();
            if (!BlasPtr) {
                LogWarning("BLAS batch: skipped a descriptor that is not ready");
                continue;
            }
            auto& Blas = static_cast<VulkanBottomLevelAccelerationStructure&>(*BlasPtr);
            const auto Sizes = Blas.GetAccelerationStructureBuildSizes();
            TotalScratchSize += Sizes.buildScratchSize;
            Candidates.push_back({.Blas = &Blas, .Sizes = Sizes});
        }
        if (Candidates.empty())
            return nullptr;

        // One shared scratch for the whole batch. The base address of a fresh
        // allocation is not guaranteed to satisfy the scratch alignment, so
        // reserve headroom and derive every offset from the actual base
        // address after allocation.
        const Uint64 ScratchAlignment = std::max<Uint64>(
            VulkanCapability::Get()
                .GetProperties<vk::PhysicalDeviceAccelerationStructurePropertiesKHR>()
                .minAccelerationStructureScratchOffsetAlignment,
            1);
        auto ScratchResult = VulkanDeviceBuffer::Create(
            Context,
            ScratchName,
            TotalScratchSize + Candidates.size() * ScratchAlignment,
            vk::BufferUsageFlagBits::eStorageBuffer);
        if (!ScratchResult)
            return std::unexpected(
                ScratchResult.error().Append("Failed to allocate the shared BLAS batch scratch buffer"));
        auto Scratch         = std::make_shared<VulkanDeviceBuffer>(std::move(*ScratchResult));
        const Uint64 BaseAddress = Scratch->GetDeviceAddress();

        const auto AlignUp = [](Uint64 Value, Uint64 Alignment) {
            return (Value + Alignment - 1) / Alignment * Alignment;
        };
        Uint64 Offset = AlignUp(BaseAddress, ScratchAlignment) - BaseAddress;

        std::vector<vk::AccelerationStructureBuildGeometryInfoKHR> Infos;
        std::vector<vk::AccelerationStructureBuildRangeInfoKHR>    Ranges;
        Infos.reserve(Candidates.size());
        Ranges.reserve(Candidates.size());
        for (auto& Candidate : Candidates) {
            auto Entry = Candidate.Blas->MaterializeBuildInfos(
                Context, Candidate.Sizes.accelerationStructureSize, *Scratch, Offset);
            if (!Entry) {
                LogError("{}", Entry.error().ToString());
                continue;
            }
            Infos.push_back(Entry->BuildInfo);
            Ranges.push_back(Entry->RangeInfo);
            Offset = AlignUp(BaseAddress + Offset + Candidate.Sizes.buildScratchSize, ScratchAlignment) - BaseAddress;
        }
        if (Infos.empty())
            return nullptr;

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
        return Scratch;
    }

    /// Null until the frame-start build batch materialized this BLAS.
    [[nodiscard]] auto GetAccelerationStructure() const -> vk::AccelerationStructureKHR {
        return m_Native ? m_Native->Get() : nullptr;
    }

  private:
    const VulkanResourceContext*            m_Context        = nullptr;
    SPtr<VulkanNativeAccelerationStructure> m_Native         = nullptr;
    vk::AccelerationStructureGeometryKHR          m_Geometry      = {};
    vk::AccelerationStructureBuildRangeInfoKHR    m_Range         = {};
    Uint32                                  m_PrimitiveCount = 0;
    bool                                    m_Built          = false;
};

/// Per-frame hollow TLAS: creation records the instance metadata only; the
/// frame-start AS phase allocates native storage sized to that instance count,
/// packs the instances, and builds. Every frame creates a fresh TLAS object,
/// so there is no capacity ring: storage, the persistent per-TLAS scratch, and
/// the single host instance buffer live and die with this object via deferred
/// deletion.
class VulkanTopLevelAccelerationStructure final : public RHITopLevelAccelerationStructure {
  public:
    explicit VulkanTopLevelAccelerationStructure(String Name) : RHITopLevelAccelerationStructure(std::move(Name)) {}

    ~VulkanTopLevelAccelerationStructure() override = default;

    VulkanTopLevelAccelerationStructure(const VulkanTopLevelAccelerationStructure&)                    = delete;
    auto operator=(const VulkanTopLevelAccelerationStructure&) -> VulkanTopLevelAccelerationStructure& = delete;
    VulkanTopLevelAccelerationStructure(VulkanTopLevelAccelerationStructure&&)                         = delete;
    auto operator=(VulkanTopLevelAccelerationStructure&&) -> VulkanTopLevelAccelerationStructure&      = delete;

    /// Hollow create: validation plus metadata copy, no device calls.
    /// Storage allocation and the build are RHI-thread confined at frame start.
    [[nodiscard]] static auto
    Create(const VulkanResourceContext& Context, StringView Name, const RHITopLevelAccelerationStructureDesc& Desc)
        -> std::expected<UPtr<RHITopLevelAccelerationStructure>, ErrorMessage> {
        if (!VulkanCapability::Get().IsRayTracingAvailable())
            return std::unexpected(ErrorMessage("Cannot create TLAS because hardware ray tracing is unavailable"));

        // step 1: every instance must reference a published BLAS payload; the
        // frame-start build re-validates native readiness.
        for (const auto& Instance : Desc.Instances) {
            if (!Instance.Blas.TryGet())
                return std::unexpected(ErrorMessage(Format("TLAS '{}' instance is missing a BLAS", Name)));
        }

        // step 2: record metadata; allocation happens at frame start.
        auto Result         = std::make_unique<VulkanTopLevelAccelerationStructure>(String(Name));
        Result->m_Device    = &Context.GetDevice();
        Result->m_Context   = &Context;
        Result->m_Instances = Desc.Instances;
        return UPtr<RHITopLevelAccelerationStructure>{std::move(Result)};
    }

    [[nodiscard]] auto GetAccelerationStructure() const -> vk::AccelerationStructureKHR {
        return m_Native ? m_Native->Get() : nullptr;
    }

    /// Build an acceleration-structure descriptor write for this TLAS.
    [[nodiscard]] auto GetWriteDescriptorSet(vk::DescriptorSet Set,
                                             Uint32            BindingIndex,
                                             bool              /*IsReadOnly*/) const
        -> vk::WriteDescriptorSet {
        m_DescriptorAccelerationStructure = GetAccelerationStructure();
        m_WriteDescriptorSetChain = {
            vk::WriteDescriptorSet{
                .dstSet          = Set,
                .dstBinding      = BindingIndex,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType  = vk::DescriptorType::eAccelerationStructureKHR,
            },
            vk::WriteDescriptorSetAccelerationStructureKHR{
                .accelerationStructureCount = 1,
                .pAccelerationStructures   = &m_DescriptorAccelerationStructure,
            },
        };
        return m_WriteDescriptorSetChain.get<vk::WriteDescriptorSet>();
    }

    /// Frame-start TLAS batch build: materialize and record each pending
    /// hollow TLAS (each into its own persistent scratch), then a build-write
    /// to ray-tracing-shader-read barrier per successful build so this frame's
    /// trace can consume the result. Recorded after the BLAS batch, so every
    /// referenced BLAS is built.
    ///
    /// Failure semantics: a per-TLAS problem is logged and skipped — the frame
    /// stays alive; there is no batch-structural resource that could fail.
    static auto Build(const VulkanResourceContext&                              Context,
                      vk::raii::CommandBuffer&                                  CmdBuffer,
                      std::span<const RHIRef<RHITopLevelAccelerationStructure>> Tlases) -> void {
        for (const auto& TlasRef : Tlases) {
            auto* TlasPtr = TlasRef.TryGet();
            if (!TlasPtr) {
                LogWarning("TLAS batch: skipped an invalid target");
                continue;
            }
            auto& Tlas = static_cast<VulkanTopLevelAccelerationStructure&>(*TlasPtr);
            if (auto R = Tlas.RecordBuild(CmdBuffer); !R) {
                LogError("{}", R.error().ToString());
                continue;
            }
            const vk::MemoryBarrier2 BuildBarrier{
                .srcStageMask  = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
                .dstStageMask  = vk::PipelineStageFlagBits2::eRayTracingShaderKHR,
                .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR,
            };
            CmdBuffer.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &BuildBarrier});
        }
    }

  private:
    /// Allocate native storage sized to the recorded instances, pack and
    /// upload them, then fully build — all in the frame-start AS phase.
    /// Recorded after the BLAS batch, so every referenced BLAS is built.
    [[nodiscard]] auto RecordBuild(const vk::raii::CommandBuffer& CmdBuf)
        -> std::expected<void, ErrorMessage> {
        // step 1: pack the recorded instances. Slot order is the canonical
        // instance order (InstanceIndex() mapping); mask/offset/flags stay at
        // their fixed defaults until a feature needs otherwise.
        std::vector<vk::AccelerationStructureInstanceKHR> VkInstances;
        VkInstances.reserve(m_Instances.size());
        for (const auto& Instance : m_Instances) {
            auto* BottomLevelPtr = Instance.Blas.TryGet();
            if (!BottomLevelPtr)
                return std::unexpected(ErrorMessage("TLAS instance is missing a BLAS"));
            const auto& Blas = static_cast<const VulkanBottomLevelAccelerationStructure&>(*BottomLevelPtr);
            if (!Blas.GetAccelerationStructure())
                return std::unexpected(ErrorMessage("TLAS instance references a BLAS that has not been built"));
            vk::AccelerationStructureInstanceKHR VkInstance{};
            VkInstance.setTransform(ToVkInstanceTransform(Instance.Transform))
                .setInstanceCustomIndex(0)
                .setMask(0xFF)
                .setInstanceShaderBindingTableRecordOffset(0)
                .setFlags(vk::GeometryInstanceFlagsKHR{})
                .setAccelerationStructureReference(
                    GetAccelerationStructureAddress(*m_Device, Blas.GetAccelerationStructure()));
            VkInstances.push_back(VkInstance);
        }

        // step 2: size and allocate fresh storage + scratch for this build.
        // The scratch is a dedicated persistent buffer owned by this TLAS (it
        // keeps future incremental updates open), so its base address must
        // satisfy the scratch alignment at creation time — unlike the BLAS
        // batch's shared buffer, there is no offset to absorb a misaligned
        // base.
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
        const std::array<Uint32, 1> PrimitiveCounts{static_cast<Uint32>(VkInstances.size())};
        const auto                  Sizes = m_Context->GetDevice().getAccelerationStructureBuildSizesKHR(
            vk::AccelerationStructureBuildTypeKHR::eDevice, SizingCI, PrimitiveCounts);
        auto Native = VulkanNativeAccelerationStructure::Create(
            *m_Context, vk::AccelerationStructureTypeKHR::eTopLevel, Sizes.accelerationStructureSize, GetName());
        if (!Native)
            return std::unexpected(Native.error().Append("Failed to allocate TLAS storage"));
        m_Native = std::move(*Native);
        const Uint64 ScratchAlignment = std::max<Uint64>(
            VulkanCapability::Get()
                .GetProperties<vk::PhysicalDeviceAccelerationStructurePropertiesKHR>()
                .minAccelerationStructureScratchOffsetAlignment,
            1);
        auto ScratchResult = VulkanDeviceBuffer::Create(
            *m_Context,
            Format("{}#Scratch", GetName()),
            Sizes.buildScratchSize,
            vk::BufferUsageFlagBits::eStorageBuffer,
            ScratchAlignment);
        if (!ScratchResult)
            return std::unexpected(ScratchResult.error().Append("Failed to allocate TLAS scratch buffer"));
        auto Scratch = std::make_shared<VulkanDeviceBuffer>(std::move(*ScratchResult));

        // step 3: stage the packed instances through one host buffer. This
        // object is single-build, so no frame-slot ring is needed; the buffer
        // retires with this object through deferred deletion.
        auto InstanceBufferResult = VulkanHostBuffer::Create(
            *m_Context,
            Format("{}#InstanceBuffer", GetName()),
            VkInstances.size() * sizeof(vk::AccelerationStructureInstanceKHR),
            vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR |
                vk::BufferUsageFlagBits::eShaderDeviceAddress);
        if (!InstanceBufferResult)
            return std::unexpected(InstanceBufferResult.error().Append("Failed to allocate TLAS instance buffer"));
        auto InstanceBuffer = std::make_shared<VulkanHostBuffer>(std::move(*InstanceBufferResult));
        if (auto R = InstanceBuffer->Upload(VkInstances.data(),
                                            VkInstances.size() * sizeof(vk::AccelerationStructureInstanceKHR));
            !R)
            return std::unexpected(R.error());
        const vk::MemoryBarrier2 InstanceUploadBarrier{
            .srcStageMask  = vk::PipelineStageFlagBits2::eHost,
            .srcAccessMask = vk::AccessFlagBits2::eHostWrite,
            .dstStageMask  = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
            .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR,
        };
        const vk::DependencyInfo InstanceUploadDependency{
            .memoryBarrierCount = 1,
            .pMemoryBarriers    = &InstanceUploadBarrier,
        };
        CmdBuf.pipelineBarrier2(InstanceUploadDependency);

        // step 4: build into the fresh storage.
        const vk::DeviceAddress InstanceAddress = InstanceBuffer->GetDeviceAddress();
        vk::AccelerationStructureGeometryInstancesDataKHR BuildData{
            .arrayOfPointers = vk::False,
            .data            = vk::DeviceOrHostAddressConstKHR{.deviceAddress = InstanceAddress},
        };
        vk::AccelerationStructureGeometryKHR BuildGeometry{
            .geometryType = vk::GeometryTypeKHR::eInstances,
            .geometry     = vk::AccelerationStructureGeometryDataKHR{.instances = BuildData},
        };
        const vk::AccelerationStructureBuildGeometryInfoKHR BuildCI{
            .type                     = vk::AccelerationStructureTypeKHR::eTopLevel,
            .flags                    = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
            .mode                     = vk::BuildAccelerationStructureModeKHR::eBuild,
            .dstAccelerationStructure = m_Native->Get(),
            .geometryCount            = 1,
            .pGeometries              = &BuildGeometry,
            .scratchData = vk::DeviceOrHostAddressKHR{.deviceAddress = Scratch->GetDeviceAddress()},
        };
        const vk::AccelerationStructureBuildRangeInfoKHR Range{.primitiveCount = static_cast<Uint32>(VkInstances.size())};
        const vk::AccelerationStructureBuildRangeInfoKHR* RangePtr = &Range;
        CmdBuf.buildAccelerationStructuresKHR(BuildCI, {RangePtr});

        // Retire with this object; deferred deletion frees them once the GPU
        // submission that used them has drained.
        m_ScratchBuffer  = std::move(Scratch);
        m_InstanceBuffer = std::move(InstanceBuffer);
        return {};
    }

    vk::raii::Device*            m_Device  = nullptr;
    const VulkanResourceContext* m_Context = nullptr;
    std::vector<RHITopLevelAccelerationStructureInstance> m_Instances = {};
    SPtr<VulkanNativeAccelerationStructure> m_Native         = nullptr;
    SPtr<VulkanHostBuffer>                  m_InstanceBuffer = nullptr;
    SPtr<VulkanDeviceBuffer>                m_ScratchBuffer  = nullptr;
    mutable vk::AccelerationStructureKHR m_DescriptorAccelerationStructure = nullptr;
    mutable vk::StructureChain<vk::WriteDescriptorSet, vk::WriteDescriptorSetAccelerationStructureKHR>
        m_WriteDescriptorSetChain = {};
};

} // namespace SoulEngine
