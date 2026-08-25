module;

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

[[nodiscard]] auto ToVkInstanceTransform(const RHIRowMajorTransform3x4& Transform) -> vk::TransformMatrixKHR {
    return vk::TransformMatrixKHR{std::array<std::array<Float32, 4>, 3>{
        std::array<Float32, 4>{Transform.M00, Transform.M01, Transform.M02, Transform.M03},
        std::array<Float32, 4>{Transform.M10, Transform.M11, Transform.M12, Transform.M13},
        std::array<Float32, 4>{Transform.M20, Transform.M21, Transform.M22, Transform.M23},
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
            Context.Device.createAccelerationStructureKHR(AccelerationStructureCI, nullptr);
        if (Result != vk::Result::eSuccess) {
            return std::unexpected(
                ErrorMessage(Format("Failed to create Vulkan acceleration structure: {}", vk::to_string(Result))));
        }

        Context.DebugUtils.SetObjectName(*RHIAccelerationStructure, Name);
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

/// Vulkan GPU payload for reusable object-space triangle geometry.
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
        if (Desc.Geometries.empty())
            return std::unexpected(ErrorMessage("Bottom-level acceleration structure requires at least one geometry"));

        std::vector<vk::AccelerationStructureGeometryKHR>       Geometries;
        std::vector<vk::AccelerationStructureBuildRangeInfoKHR> BuildRanges;
        std::vector<Uint32>                                     PrimitiveCounts;
        Geometries.reserve(Desc.Geometries.size());
        BuildRanges.reserve(Desc.Geometries.size());
        PrimitiveCounts.reserve(Desc.Geometries.size());

        for (const auto& GeometryDesc : Desc.Geometries) {
            auto* VertexBufferPtr = GeometryDesc.VertexBufferRef.TryGet();
            auto* IndexBufferPtr  = GeometryDesc.IndexBufferRef.TryGet();
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

            vk::AccelerationStructureGeometryTrianglesDataKHR Triangles{
                .vertexFormat  = vk::Format::eR32G32B32Sfloat,
                .vertexData    = vk::DeviceOrHostAddressConstKHR{.deviceAddress = VertexAddress},
                .vertexStride  = VertexBuf.GetStride(),
                .maxVertex     = static_cast<Uint32>(VertexBuf.GetVertexCount() - 1),
                .indexType     = vk::IndexType::eUint32,
                .indexData     = vk::DeviceOrHostAddressConstKHR{.deviceAddress = IndexAddress},
                .transformData = vk::DeviceOrHostAddressConstKHR{.deviceAddress = 0},
            };
            Geometries.push_back(vk::AccelerationStructureGeometryKHR{
                .geometryType = vk::GeometryTypeKHR::eTriangles,
                .geometry     = vk::AccelerationStructureGeometryDataKHR{.triangles = Triangles},
                .flags        = vk::GeometryFlagBitsKHR::eOpaque,
            });
            const Uint32 PrimitiveCount = static_cast<Uint32>(IndexBuf.GetIndexCount() / 3);
            PrimitiveCounts.push_back(PrimitiveCount);
            BuildRanges.push_back(vk::AccelerationStructureBuildRangeInfoKHR{
                .primitiveCount  = PrimitiveCount,
                .primitiveOffset = 0,
                .firstVertex     = 0,
                .transformOffset = 0,
            });
        }

        vk::AccelerationStructureBuildGeometryInfoKHR BuildGeometryCI{
            .type          = vk::AccelerationStructureTypeKHR::eBottomLevel,
            .flags         = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
            .mode          = vk::BuildAccelerationStructureModeKHR::eBuild,
            .geometryCount = static_cast<Uint32>(Geometries.size()),
            .pGeometries   = Geometries.data(),
        };
        const auto Sizes = Context.Device.getAccelerationStructureBuildSizesKHR(
            vk::AccelerationStructureBuildTypeKHR::eDevice, BuildGeometryCI, PrimitiveCounts);
        auto Native = VulkanNativeAccelerationStructure::Create(
            Context, vk::AccelerationStructureTypeKHR::eBottomLevel, Sizes.accelerationStructureSize, Name);
        if (!Native)
            return std::unexpected(Native.error().Append("Failed to allocate BLAS storage"));
        auto ScratchResult = VulkanDeviceBuffer::Create(
            Context, Format("{}#Scratch", Name), Sizes.buildScratchSize, vk::BufferUsageFlagBits::eStorageBuffer);
        if (!ScratchResult)
            return std::unexpected(ScratchResult.error().Append("Failed to allocate BLAS scratch buffer"));
        auto Scratch = std::make_shared<VulkanDeviceBuffer>(std::move(*ScratchResult));

        BuildGeometryCI.dstAccelerationStructure = (*Native)->Get();
        BuildGeometryCI.scratchData              = vk::DeviceOrHostAddressKHR{
            .deviceAddress = Scratch->GetDeviceAddress(),
        };
        if (BuildGeometryCI.scratchData.deviceAddress == 0)
            return std::unexpected(ErrorMessage("BLAS scratch buffer has no device address"));
        const vk::AccelerationStructureBuildRangeInfoKHR* BuildRangePtr = BuildRanges.data();
        if (auto R = Context.Immediate.SubmitAndWait(VulkanImmediateQueue::Graphics,
                                                     vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
                                                     [&](const vk::raii::CommandBuffer& CmdBuf) {
                                                         CmdBuf.buildAccelerationStructuresKHR(BuildGeometryCI,
                                                                                               {BuildRangePtr});
                                                     });
            !R) {
            return std::unexpected(R.error().Append("Failed to build BLAS"));
        }

        auto Result      = std::make_unique<VulkanBottomLevelAccelerationStructure>(String(Name));
        Result->m_Native = std::move(*Native);
        return UPtr<RHIBottomLevelAccelerationStructure>{std::move(Result)};
    }

    [[nodiscard]] auto GetAccelerationStructure() const -> vk::AccelerationStructureKHR {
        return m_Native->Get();
    }

  private:
    SPtr<VulkanNativeAccelerationStructure> m_Native = nullptr;
};

/// Vulkan persistent TLAS allocation. Instance uploads and builds are recorded by the command encoder.
class VulkanTopLevelAccelerationStructure final : public RHITopLevelAccelerationStructure {
  public:
    explicit VulkanTopLevelAccelerationStructure(String Name) : RHITopLevelAccelerationStructure(std::move(Name)) {}

    ~VulkanTopLevelAccelerationStructure() override = default;

    VulkanTopLevelAccelerationStructure(const VulkanTopLevelAccelerationStructure&)                    = delete;
    auto operator=(const VulkanTopLevelAccelerationStructure&) -> VulkanTopLevelAccelerationStructure& = delete;

    [[nodiscard]] static auto
    Create(const VulkanResourceContext& Context, StringView Name, const RHITopLevelAccelerationStructureDesc& Desc)
        -> std::expected<UPtr<RHITopLevelAccelerationStructure>, ErrorMessage> {
        if (!VulkanCapability::Get().IsRayTracingAvailable())
            return std::unexpected(ErrorMessage("Cannot create TLAS because hardware ray tracing is unavailable"));
        if (Desc.InitialInstanceCapacity == 0)
            return std::unexpected(ErrorMessage("Top-level acceleration structure requires non-zero initial capacity"));

        vk::AccelerationStructureGeometryInstancesDataKHR Instances{
            .arrayOfPointers = vk::False,
            .data            = vk::DeviceOrHostAddressConstKHR{.deviceAddress = 0},
        };
        vk::AccelerationStructureGeometryKHR Geometry{
            .geometryType = vk::GeometryTypeKHR::eInstances,
            .geometry     = vk::AccelerationStructureGeometryDataKHR{.instances = Instances},
        };
        vk::AccelerationStructureBuildGeometryInfoKHR BuildGeometryCI{
            .type          = vk::AccelerationStructureTypeKHR::eTopLevel,
            .flags         = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace |
                             vk::BuildAccelerationStructureFlagBitsKHR::eAllowUpdate,
            .mode          = vk::BuildAccelerationStructureModeKHR::eBuild,
            .geometryCount = 1,
            .pGeometries   = &Geometry,
        };
        const std::array<Uint32, 1> PrimitiveCounts{Desc.InitialInstanceCapacity};
        const auto                  Sizes = Context.Device.getAccelerationStructureBuildSizesKHR(
            vk::AccelerationStructureBuildTypeKHR::eDevice, BuildGeometryCI, PrimitiveCounts);
        auto Native = VulkanNativeAccelerationStructure::Create(
            Context, vk::AccelerationStructureTypeKHR::eTopLevel, Sizes.accelerationStructureSize, Name);
        if (!Native)
            return std::unexpected(Native.error().Append("Failed to allocate TLAS storage"));

        auto InstanceBufferResult = VulkanHostBuffer::Create(
            Context,
            Format("{}#InstanceBuffer", Name),
            static_cast<Uint64>(Desc.InitialInstanceCapacity) * sizeof(vk::AccelerationStructureInstanceKHR),
            vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR |
                vk::BufferUsageFlagBits::eShaderDeviceAddress);
        if (!InstanceBufferResult)
            return std::unexpected(InstanceBufferResult.error().Append("Failed to allocate TLAS instance buffer"));
        auto InstanceBuffer      = std::make_shared<VulkanHostBuffer>(std::move(*InstanceBufferResult));
        auto ScratchBufferResult = VulkanDeviceBuffer::Create(
            Context, Format("{}#Scratch", Name), Sizes.buildScratchSize, vk::BufferUsageFlagBits::eStorageBuffer);
        if (!ScratchBufferResult)
            return std::unexpected(ScratchBufferResult.error().Append("Failed to allocate TLAS scratch buffer"));
        auto ScratchBuffer = std::make_shared<VulkanDeviceBuffer>(std::move(*ScratchBufferResult));

        auto Result                = std::make_unique<VulkanTopLevelAccelerationStructure>(String(Name));
        Result->m_Context          = &Context;
        Result->m_Device           = &Context.Device;
        Result->m_Allocator        = Context.Allocator;
        Result->m_Native           = std::move(*Native);
        Result->m_InstanceBuffer   = std::move(InstanceBuffer);
        Result->m_ScratchBuffer    = std::move(ScratchBuffer);
        Result->m_InstanceCapacity = Desc.InitialInstanceCapacity;
        return UPtr<RHITopLevelAccelerationStructure>{std::move(Result)};
    }

    [[nodiscard]] auto GetInstanceCapacity() const -> Uint32 {
        return m_InstanceCapacity;
    }

    [[nodiscard]] auto GetAccelerationStructure() const -> vk::AccelerationStructureKHR {
        return m_Native->Get();
    }

    [[nodiscard]] auto RecordBuild(const vk::raii::CommandBuffer&                    CmdBuf,
                                   std::span<const RHIAccelerationStructureInstance> Instances,
                                   RHITopLevelAccelerationStructureBuildMode         Mode,
                                   std::vector<std::function<void()>>*               RetiredPayloads)
        -> std::expected<void, ErrorMessage> {
        if (Instances.empty())
            return std::unexpected(ErrorMessage("TLAS requires at least one instance"));
        if (Instances.size() > m_InstanceCapacity) {
            if (auto R = GrowTo(static_cast<Uint32>(Instances.size()), RetiredPayloads); !R)
                return std::unexpected(R.error());
        }
        std::vector<vk::AccelerationStructureInstanceKHR> VkInstances;
        VkInstances.reserve(Instances.size());
        for (const auto& Instance : Instances) {
            auto* BottomLevelPtr = Instance.BottomLevelRef.TryGet();
            if (!BottomLevelPtr)
                return std::unexpected(ErrorMessage("TLAS instance is missing a BLAS"));
            const auto& Blas = static_cast<const VulkanBottomLevelAccelerationStructure&>(*BottomLevelPtr);
            vk::AccelerationStructureInstanceKHR VkInstance{};
            const auto                           Flags =
                Instance.Flags == RHIAccelerationStructureInstanceFlags::DisableTriangleCulling
                    ? vk::GeometryInstanceFlagsKHR{vk::GeometryInstanceFlagBitsKHR::eTriangleFacingCullDisable}
                    : vk::GeometryInstanceFlagsKHR{};
            VkInstance.setTransform(ToVkInstanceTransform(Instance.Transform))
                .setInstanceCustomIndex(Instance.CustomIndex)
                .setMask(Instance.Mask)
                .setInstanceShaderBindingTableRecordOffset(Instance.HitGroupIndex)
                .setFlags(Flags)
                .setAccelerationStructureReference(
                    GetAccelerationStructureAddress(*m_Device, Blas.GetAccelerationStructure()));
            VkInstances.push_back(VkInstance);
        }
        if (auto R = m_InstanceBuffer->Upload(VkInstances.data(),
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
        const vk::DeviceAddress                           InstanceAddress = m_InstanceBuffer->GetDeviceAddress();
        vk::AccelerationStructureGeometryInstancesDataKHR InstanceData{
            .arrayOfPointers = vk::False,
            .data            = vk::DeviceOrHostAddressConstKHR{.deviceAddress = InstanceAddress},
        };
        vk::AccelerationStructureGeometryKHR Geometry{
            .geometryType = vk::GeometryTypeKHR::eInstances,
            .geometry     = vk::AccelerationStructureGeometryDataKHR{.instances = InstanceData},
        };
        const bool bCanUpdate =
            Mode != RHITopLevelAccelerationStructureBuildMode::Build && m_LastInstanceCount == Instances.size();
        vk::AccelerationStructureBuildGeometryInfoKHR BuildGeometryCI{
            .type                     = vk::AccelerationStructureTypeKHR::eTopLevel,
            .flags                    = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace |
                                        vk::BuildAccelerationStructureFlagBitsKHR::eAllowUpdate,
            .mode                     = bCanUpdate ? vk::BuildAccelerationStructureModeKHR::eUpdate
                                                   : vk::BuildAccelerationStructureModeKHR::eBuild,
            .srcAccelerationStructure = bCanUpdate ? m_Native->Get() : nullptr,
            .dstAccelerationStructure = m_Native->Get(),
            .geometryCount            = 1,
            .pGeometries              = &Geometry,
            .scratchData = vk::DeviceOrHostAddressKHR{.deviceAddress = m_ScratchBuffer->GetDeviceAddress()},
        };
        const vk::AccelerationStructureBuildRangeInfoKHR Range{.primitiveCount = static_cast<Uint32>(Instances.size())};
        const vk::AccelerationStructureBuildRangeInfoKHR* RangePtr = &Range;
        CmdBuf.buildAccelerationStructuresKHR(BuildGeometryCI, {RangePtr});
        m_LastInstanceCount = static_cast<Uint32>(Instances.size());
        return {};
    }

  private:
    [[nodiscard]] auto GrowTo(Uint32 RequiredCapacity, std::vector<std::function<void()>>* RetiredPayloads)
        -> std::expected<void, ErrorMessage> {
        const Uint32 NewCapacity = (std::max)(RequiredCapacity, m_InstanceCapacity * 2);
        vk::AccelerationStructureGeometryInstancesDataKHR InstanceData{
            .arrayOfPointers = vk::False,
            .data            = vk::DeviceOrHostAddressConstKHR{.deviceAddress = 0},
        };
        vk::AccelerationStructureGeometryKHR Geometry{
            .geometryType = vk::GeometryTypeKHR::eInstances,
            .geometry     = vk::AccelerationStructureGeometryDataKHR{.instances = InstanceData},
        };
        vk::AccelerationStructureBuildGeometryInfoKHR BuildGeometryCI{
            .type          = vk::AccelerationStructureTypeKHR::eTopLevel,
            .flags         = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace |
                             vk::BuildAccelerationStructureFlagBitsKHR::eAllowUpdate,
            .mode          = vk::BuildAccelerationStructureModeKHR::eBuild,
            .geometryCount = 1,
            .pGeometries   = &Geometry,
        };
        const std::array<Uint32, 1> PrimitiveCounts{NewCapacity};
        const auto                  Sizes = m_Device->getAccelerationStructureBuildSizesKHR(
            vk::AccelerationStructureBuildTypeKHR::eDevice, BuildGeometryCI, PrimitiveCounts);
        auto Native = VulkanNativeAccelerationStructure::Create(*m_Context,
                                                                vk::AccelerationStructureTypeKHR::eTopLevel,
                                                                Sizes.accelerationStructureSize,
                                                                Format("{}::Growth", GetName()));
        if (!Native)
            return std::unexpected(Native.error().Append("Failed to grow TLAS storage"));
        auto InstanceBufferResult =
            VulkanHostBuffer::Create(*m_Context,
                                     Format("{}#InstanceBuffer", GetName()),
                                     static_cast<Uint64>(NewCapacity) * sizeof(vk::AccelerationStructureInstanceKHR),
                                     vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR |
                                         vk::BufferUsageFlagBits::eShaderDeviceAddress);
        if (!InstanceBufferResult)
            return std::unexpected(InstanceBufferResult.error().Append("Failed to grow TLAS instance buffer"));
        auto InstanceBuffer      = std::make_shared<VulkanHostBuffer>(std::move(*InstanceBufferResult));
        auto ScratchBufferResult = VulkanDeviceBuffer::Create(*m_Context,
                                                              Format("{}#Scratch", GetName()),
                                                              Sizes.buildScratchSize,
                                                              vk::BufferUsageFlagBits::eStorageBuffer);
        if (!ScratchBufferResult)
            return std::unexpected(ScratchBufferResult.error().Append("Failed to grow TLAS scratch buffer"));
        auto ScratchBuffer = std::make_shared<VulkanDeviceBuffer>(std::move(*ScratchBufferResult));

        auto OldNative         = std::move(m_Native);
        auto OldInstanceBuffer = std::move(m_InstanceBuffer);
        auto OldScratchBuffer  = std::move(m_ScratchBuffer);
        m_Native               = std::move(*Native);
        m_InstanceBuffer       = std::move(InstanceBuffer);
        m_ScratchBuffer        = std::move(ScratchBuffer);
        m_InstanceCapacity     = NewCapacity;
        m_LastInstanceCount    = 0;
        RetiredPayloads->emplace_back([Native         = std::move(OldNative),
                                       InstanceBuffer = std::move(OldInstanceBuffer),
                                       ScratchBuffer  = std::move(OldScratchBuffer)]() {});
        return {};
    }

    const VulkanResourceContext*            m_Context           = nullptr;
    vk::raii::Device*                       m_Device            = nullptr;
    VmaAllocator                            m_Allocator         = nullptr;
    SPtr<VulkanNativeAccelerationStructure> m_Native            = nullptr;
    SPtr<VulkanHostBuffer>                  m_InstanceBuffer    = nullptr;
    SPtr<VulkanDeviceBuffer>                m_ScratchBuffer     = nullptr;
    Uint32                                  m_InstanceCapacity  = 0;
    Uint32                                  m_LastInstanceCount = 0;
};

} // namespace SoulEngine
