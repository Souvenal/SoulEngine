module;

#include <vk_mem_alloc.h>

export module Vulkan:AccelerationStructure;

import Core;
import RHI;
import vulkan;
import std;

import :Buffer;
import :Capability;
import :DeletionQueue;
import :ImmediateContext;
import :TransferCompletionQueue;

namespace SoulEngine {

namespace {

[[nodiscard]] auto ToVkBuildFlags(RHIAccelerationStructureBuildFlags Flags) -> vk::BuildAccelerationStructureFlagsKHR {
    auto Result = vk::BuildAccelerationStructureFlagsKHR{};
    const auto Raw = static_cast<Uint32>(Flags);
    if ((Raw & static_cast<Uint32>(RHIAccelerationStructureBuildFlags::PreferFastTrace)) != 0)
        Result |= vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
    if ((Raw & static_cast<Uint32>(RHIAccelerationStructureBuildFlags::PreferFastBuild)) != 0)
        Result |= vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild;
    if ((Raw & static_cast<Uint32>(RHIAccelerationStructureBuildFlags::AllowUpdate)) != 0)
        Result |= vk::BuildAccelerationStructureFlagBitsKHR::eAllowUpdate;
    return Result;
}

[[nodiscard]] auto ToVkGeometryFlags(RHIAccelerationStructureGeometryFlags Flags) -> vk::GeometryFlagsKHR {
    auto Result = vk::GeometryFlagsKHR{};
    if ((static_cast<Uint32>(Flags) & static_cast<Uint32>(RHIAccelerationStructureGeometryFlags::Opaque)) != 0)
        Result |= vk::GeometryFlagBitsKHR::eOpaque;
    return Result;
}

[[nodiscard]] auto ToVkInstanceTransform(const RHIRowMajorTransform3x4& Transform) -> vk::TransformMatrixKHR {
    return vk::TransformMatrixKHR{std::array<std::array<Float32, 4>, 3>{
        std::array<Float32, 4>{Transform.M00, Transform.M01, Transform.M02, Transform.M03},
        std::array<Float32, 4>{Transform.M10, Transform.M11, Transform.M12, Transform.M13},
        std::array<Float32, 4>{Transform.M20, Transform.M21, Transform.M22, Transform.M23},
    }};
}

[[nodiscard]] auto ToVkVertexFormat(RHIFormat FormatValue) -> std::expected<vk::Format, ErrorMessage> {
    switch (FormatValue) {
    case RHIFormat::R32G32B32_SFLOAT:
        return vk::Format::eR32G32B32Sfloat;
    default:
        return std::unexpected(ErrorMessage("Acceleration-structure geometry requires R32G32B32_SFLOAT positions"));
    }
}

class VulkanVmaBuffer final {
  public:
    VulkanVmaBuffer() = default;
    ~VulkanVmaBuffer() {
        if (m_Allocation)
            vmaDestroyBuffer(m_Allocator, static_cast<VkBuffer>(m_Buffer), m_Allocation);
    }

    VulkanVmaBuffer(const VulkanVmaBuffer&)                    = delete;
    auto operator=(const VulkanVmaBuffer&) -> VulkanVmaBuffer& = delete;
    VulkanVmaBuffer(VulkanVmaBuffer&&)                         = delete;
    auto operator=(VulkanVmaBuffer&&) -> VulkanVmaBuffer&      = delete;

    [[nodiscard]] static auto Create(Uint64 Size, vk::BufferUsageFlags Usage, VmaAllocator Allocator)
        -> std::expected<SPtr<VulkanVmaBuffer>, ErrorMessage> {
        if (Size == 0)
            return std::unexpected(ErrorMessage("Acceleration-structure buffer size must be non-zero"));

        auto Result       = std::make_shared<VulkanVmaBuffer>();
        Result->m_Allocator = Allocator;
        Result->m_Size      = Size;
        vk::BufferCreateInfo BufferCI{
            .size        = Size,
            .usage       = Usage | vk::BufferUsageFlagBits::eShaderDeviceAddress,
            .sharingMode = vk::SharingMode::eExclusive,
        };
        VmaAllocationCreateInfo AllocationCI{
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        };
        if (vmaCreateBuffer(Allocator,
                            reinterpret_cast<VkBufferCreateInfo*>(&BufferCI),
                            &AllocationCI,
                            reinterpret_cast<VkBuffer*>(&Result->m_Buffer),
                            &Result->m_Allocation,
                            nullptr) != VK_SUCCESS) {
            return std::unexpected(ErrorMessage("Failed to allocate Vulkan acceleration-structure buffer"));
        }
        return Result;
    }

    [[nodiscard]] static auto CreateHostVisible(Uint64 Size, vk::BufferUsageFlags Usage, VmaAllocator Allocator)
        -> std::expected<SPtr<VulkanVmaBuffer>, ErrorMessage> {
        if (Size == 0)
            return std::unexpected(ErrorMessage("Host-visible acceleration-structure buffer size must be non-zero"));

        auto Result       = std::make_shared<VulkanVmaBuffer>();
        Result->m_Allocator = Allocator;
        Result->m_Size      = Size;
        vk::BufferCreateInfo BufferCI{
            .size        = Size,
            .usage       = Usage | vk::BufferUsageFlagBits::eShaderDeviceAddress,
            .sharingMode = vk::SharingMode::eExclusive,
        };
        VmaAllocationCreateInfo AllocationCI{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };
        if (vmaCreateBuffer(Allocator,
                            reinterpret_cast<VkBufferCreateInfo*>(&BufferCI),
                            &AllocationCI,
                            reinterpret_cast<VkBuffer*>(&Result->m_Buffer),
                            &Result->m_Allocation,
                            nullptr) != VK_SUCCESS) {
            return std::unexpected(ErrorMessage("Failed to allocate host-visible acceleration-structure buffer"));
        }
        return Result;
    }

    [[nodiscard]] auto Upload(const void* Data, Uint64 Size) const -> std::expected<void, ErrorMessage> {
        if (!Data || Size == 0 || Size > m_Size)
            return std::unexpected(ErrorMessage("Invalid acceleration-structure buffer upload range"));
        if (vmaCopyMemoryToAllocation(m_Allocator, Data, m_Allocation, 0, Size) != VK_SUCCESS)
            return std::unexpected(ErrorMessage("Failed to upload acceleration-structure instance data"));
        return {};
    }

    [[nodiscard]] auto Get() const -> vk::Buffer {
        return m_Buffer;
    }
    [[nodiscard]] auto GetSize() const -> Uint64 {
        return m_Size;
    }

  private:
    VmaAllocator  m_Allocator  = nullptr;
    vk::Buffer    m_Buffer     = nullptr;
    VmaAllocation m_Allocation = nullptr;
    Uint64        m_Size       = 0;
};

class VulkanNativeAccelerationStructure final {
  public:
    VulkanNativeAccelerationStructure() = default;

    [[nodiscard]] static auto Create(vk::raii::Device&                 Device,
                                     VmaAllocator                       Allocator,
                                     vk::AccelerationStructureTypeKHR  Type,
                                     Uint64                             Size)
        -> std::expected<SPtr<VulkanNativeAccelerationStructure>, ErrorMessage> {
        auto Storage = VulkanVmaBuffer::Create(Size, vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR, Allocator);
        if (!Storage)
            return std::unexpected(Storage.error());

        vk::AccelerationStructureCreateInfoKHR AccelerationStructureCI{
            .buffer = (*Storage)->Get(),
            .size   = Size,
            .type   = Type,
        };
        auto [Result, RHIAccelerationStructure] = Device.createAccelerationStructureKHR(AccelerationStructureCI, nullptr);
        if (Result != vk::Result::eSuccess) {
            return std::unexpected(ErrorMessage(Format(
                "Failed to create Vulkan acceleration structure: {}", vk::to_string(Result))));
        }

        auto Native = std::make_shared<VulkanNativeAccelerationStructure>();
        Native->m_Storage = std::move(*Storage);
        Native->m_AccelerationStructure = std::make_shared<vk::raii::AccelerationStructureKHR>(std::move(RHIAccelerationStructure));
        return Native;
    }

    [[nodiscard]] auto Get() const -> vk::AccelerationStructureKHR {
        return *(*m_AccelerationStructure);
    }

  private:
    SPtr<VulkanVmaBuffer>                              m_Storage = nullptr;
    SPtr<vk::raii::AccelerationStructureKHR>     m_AccelerationStructure = nullptr;
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
    VulkanBottomLevelAccelerationStructure() = default;

    ~VulkanBottomLevelAccelerationStructure() override {
        if (m_DeletionQueue)
            m_DeletionQueue->Enqueue(GetLastUsageToken(), [Native = m_Native]() {});
    }

    VulkanBottomLevelAccelerationStructure(const VulkanBottomLevelAccelerationStructure&)                    = delete;
    auto operator=(const VulkanBottomLevelAccelerationStructure&) -> VulkanBottomLevelAccelerationStructure& = delete;

    [[nodiscard]] static auto Create(vk::raii::Device&                                   Device,
                                     VmaAllocator                                         Allocator,
                                     VulkanImmediateContext&                                    BuildContext,
                                     VulkanTransferCompletionQueue&                             CompletionQueue,
                                     VulkanDeletionQueue&                                       DeletionQueueRef,
                                     const RHIBottomLevelAccelerationStructureDesc&     Desc)
        -> std::expected<UPtr<RHIBottomLevelAccelerationStructure>, ErrorMessage> {
        if (!VulkanCapability::Get().GetRayTracingSupport().Available)
            return std::unexpected(ErrorMessage("Cannot create BLAS because hardware ray tracing is unavailable"));
        if (Desc.Geometries.empty())
            return std::unexpected(ErrorMessage("Bottom-level acceleration structure requires at least one geometry"));

        std::vector<vk::AccelerationStructureGeometryKHR> Geometries;
        std::vector<vk::AccelerationStructureBuildRangeInfoKHR> BuildRanges;
        std::vector<Uint32> PrimitiveCounts;
        Geometries.reserve(Desc.Geometries.size());
        BuildRanges.reserve(Desc.Geometries.size());
        PrimitiveCounts.reserve(Desc.Geometries.size());

        for (const auto& GeometryDesc : Desc.Geometries) {
            if (!GeometryDesc.VertexBufferPtr || !GeometryDesc.IndexBufferPtr) {
                return std::unexpected(ErrorMessage("BLAS triangle geometry requires both vertex and index buffers"));
            }
            if (GeometryDesc.VertexCount == 0 || GeometryDesc.IndexCount == 0 || GeometryDesc.IndexCount % 3 != 0) {
                return std::unexpected(ErrorMessage("BLAS triangle geometry requires non-empty triangle indices"));
            }
            if (GeometryDesc.VertexCount > std::numeric_limits<Uint32>::max() ||
                GeometryDesc.IndexCount / 3 > std::numeric_limits<Uint32>::max()) {
                return std::unexpected(ErrorMessage("BLAS geometry count exceeds Vulkan Uint32 limits"));
            }
            if (GeometryDesc.IndexType != RHIAccelerationStructureIndexType::Uint32)
                return std::unexpected(ErrorMessage("BLAS currently supports only Uint32 index buffers"));

            auto VertexFormat = ToVkVertexFormat(GeometryDesc.VertexFormat);
            if (!VertexFormat)
                return std::unexpected(VertexFormat.error());
            auto& VertexBuf = static_cast<const VulkanVertexBuffer&>(*GeometryDesc.VertexBufferPtr);
            auto& IndexBuf  = static_cast<const VulkanIndexBuffer&>(*GeometryDesc.IndexBufferPtr);
            const vk::DeviceAddress VertexAddress =
                Device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = VertexBuf.GetVkBuffer()});
            const vk::DeviceAddress IndexAddress =
                Device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = IndexBuf.GetVkBuffer()});
            if (VertexAddress == 0 || IndexAddress == 0)
                return std::unexpected(ErrorMessage("BLAS geometry buffer has no device address"));

            vk::AccelerationStructureGeometryTrianglesDataKHR Triangles{
                .vertexFormat  = *VertexFormat,
                .vertexData    = vk::DeviceOrHostAddressConstKHR{.deviceAddress = VertexAddress},
                .vertexStride  = GeometryDesc.VertexStride,
                .maxVertex     = static_cast<Uint32>(GeometryDesc.VertexCount - 1),
                .indexType     = vk::IndexType::eUint32,
                .indexData     = vk::DeviceOrHostAddressConstKHR{.deviceAddress = IndexAddress},
                .transformData = vk::DeviceOrHostAddressConstKHR{.deviceAddress = 0},
            };
            Geometries.push_back(vk::AccelerationStructureGeometryKHR{
                .geometryType = vk::GeometryTypeKHR::eTriangles,
                .geometry     = vk::AccelerationStructureGeometryDataKHR{.triangles = Triangles},
                .flags        = ToVkGeometryFlags(GeometryDesc.Flags),
            });
            const Uint32 PrimitiveCount = static_cast<Uint32>(GeometryDesc.IndexCount / 3);
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
            .flags         = ToVkBuildFlags(Desc.BuildFlags),
            .mode          = vk::BuildAccelerationStructureModeKHR::eBuild,
            .geometryCount = static_cast<Uint32>(Geometries.size()),
            .pGeometries   = Geometries.data(),
        };
        const auto Sizes = Device.getAccelerationStructureBuildSizesKHR(
            vk::AccelerationStructureBuildTypeKHR::eDevice, BuildGeometryCI, PrimitiveCounts);
        auto Native = VulkanNativeAccelerationStructure::Create(
            Device, Allocator, vk::AccelerationStructureTypeKHR::eBottomLevel, Sizes.accelerationStructureSize);
        if (!Native)
            return std::unexpected(Native.error().Append("Failed to allocate BLAS storage"));
        auto Scratch = VulkanVmaBuffer::Create(
            Sizes.buildScratchSize, vk::BufferUsageFlagBits::eStorageBuffer, Allocator);
        if (!Scratch)
            return std::unexpected(Scratch.error().Append("Failed to allocate BLAS scratch buffer"));

        // Vertex and index buffers are uploaded on the transfer queue. A BLAS build
        // is submitted on the graphics queue, so retire all outstanding uploads before
        // submitting this one-time graphics build rather than racing queue families.
        if (auto R = CompletionQueue.Drain(); !R)
            return std::unexpected(R.error().Append("Failed to wait for BLAS geometry uploads"));

        BuildGeometryCI.dstAccelerationStructure = (*Native)->Get();
        BuildGeometryCI.scratchData = vk::DeviceOrHostAddressKHR{
            .deviceAddress = Device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = (*Scratch)->Get()}),
        };
        if (BuildGeometryCI.scratchData.deviceAddress == 0)
            return std::unexpected(ErrorMessage("BLAS scratch buffer has no device address"));
        const vk::AccelerationStructureBuildRangeInfoKHR* BuildRangePtr = BuildRanges.data();
        auto BuildToken = BuildContext.SubmitTransfer([&](const vk::raii::CommandBuffer& CmdBuf) {
            CmdBuf.buildAccelerationStructuresKHR(BuildGeometryCI, {BuildRangePtr});
        });
        if (!BuildToken)
            return std::unexpected(BuildToken.error().Append("Failed to submit BLAS build"));
        CompletionQueue.EnqueueCallback(*BuildToken, [ScratchBuffer = *Scratch]() {});
        if (auto R = CompletionQueue.Drain(); !R)
            return std::unexpected(R.error().Append("Failed to wait for BLAS build completion"));

        auto Result = std::make_unique<VulkanBottomLevelAccelerationStructure>();
        Result->m_Native = std::move(*Native);
        Result->m_DeletionQueue = &DeletionQueueRef;
        return UPtr<RHIBottomLevelAccelerationStructure>{std::move(Result)};
    }

    [[nodiscard]] auto GetAccelerationStructure() const -> vk::AccelerationStructureKHR {
        return m_Native->Get();
    }

  private:
    SPtr<VulkanNativeAccelerationStructure> m_Native = nullptr;
    VulkanDeletionQueue*                    m_DeletionQueue = nullptr;
};

/// Vulkan persistent TLAS allocation. Instance uploads and builds are recorded by the command encoder.
class VulkanTopLevelAccelerationStructure final : public RHITopLevelAccelerationStructure {
  public:
    VulkanTopLevelAccelerationStructure() = default;

    ~VulkanTopLevelAccelerationStructure() override {
        if (m_DeletionQueue)
            m_DeletionQueue->Enqueue(GetLastUsageToken(), [Native = m_Native]() {});
    }

    VulkanTopLevelAccelerationStructure(const VulkanTopLevelAccelerationStructure&)                    = delete;
    auto operator=(const VulkanTopLevelAccelerationStructure&) -> VulkanTopLevelAccelerationStructure& = delete;

    [[nodiscard]] static auto Create(vk::raii::Device&                                Device,
                                     VmaAllocator                                      Allocator,
                                     VulkanDeletionQueue&                                    DeletionQueueRef,
                                     const RHITopLevelAccelerationStructureDesc&     Desc)
        -> std::expected<UPtr<RHITopLevelAccelerationStructure>, ErrorMessage> {
        if (!VulkanCapability::Get().GetRayTracingSupport().Available)
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
            .flags         = ToVkBuildFlags(Desc.BuildFlags),
            .mode          = vk::BuildAccelerationStructureModeKHR::eBuild,
            .geometryCount = 1,
            .pGeometries   = &Geometry,
        };
        const std::array<Uint32, 1> PrimitiveCounts{Desc.InitialInstanceCapacity};
        const auto Sizes = Device.getAccelerationStructureBuildSizesKHR(
            vk::AccelerationStructureBuildTypeKHR::eDevice, BuildGeometryCI, PrimitiveCounts);
        auto Native = VulkanNativeAccelerationStructure::Create(
            Device, Allocator, vk::AccelerationStructureTypeKHR::eTopLevel, Sizes.accelerationStructureSize);
        if (!Native)
            return std::unexpected(Native.error().Append("Failed to allocate TLAS storage"));

        auto InstanceBuffer = VulkanVmaBuffer::CreateHostVisible(
            static_cast<Uint64>(Desc.InitialInstanceCapacity) * sizeof(vk::AccelerationStructureInstanceKHR),
            vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR,
            Allocator);
        if (!InstanceBuffer)
            return std::unexpected(InstanceBuffer.error().Append("Failed to allocate TLAS instance buffer"));
        auto ScratchBuffer = VulkanVmaBuffer::Create(Sizes.buildScratchSize, vk::BufferUsageFlagBits::eStorageBuffer, Allocator);
        if (!ScratchBuffer)
            return std::unexpected(ScratchBuffer.error().Append("Failed to allocate TLAS scratch buffer"));

        auto Result = std::make_unique<VulkanTopLevelAccelerationStructure>();
        Result->m_Device = &Device;
        Result->m_Allocator = Allocator;
        Result->m_Native = std::move(*Native);
        Result->m_InstanceBuffer = std::move(*InstanceBuffer);
        Result->m_ScratchBuffer = std::move(*ScratchBuffer);
        Result->m_InstanceCapacity = Desc.InitialInstanceCapacity;
        Result->m_BuildFlags = Desc.BuildFlags;
        Result->m_DeletionQueue = &DeletionQueueRef;
        return UPtr<RHITopLevelAccelerationStructure>{std::move(Result)};
    }

    [[nodiscard]] auto GetInstanceCapacity() const -> Uint32 {
        return m_InstanceCapacity;
    }

    [[nodiscard]] auto GetAccelerationStructure() const -> vk::AccelerationStructureKHR {
        return m_Native->Get();
    }

    [[nodiscard]] auto RecordBuild(const vk::raii::CommandBuffer& CmdBuf,
                                   std::span<const RHIAccelerationStructureInstance> Instances,
                                   RHITopLevelAccelerationStructureBuildMode Mode) -> std::expected<void, ErrorMessage> {
        if (Instances.empty())
            return std::unexpected(ErrorMessage("TLAS requires at least one instance"));
        if (Instances.size() > m_InstanceCapacity) {
            if (auto R = GrowTo(static_cast<Uint32>(Instances.size())); !R)
                return std::unexpected(R.error());
        }
        std::vector<vk::AccelerationStructureInstanceKHR> VkInstances;
        VkInstances.reserve(Instances.size());
        for (const auto& Instance : Instances) {
            if (!Instance.BottomLevelPtr)
                return std::unexpected(ErrorMessage("TLAS instance is missing a BLAS"));
            const auto& Blas = static_cast<const VulkanBottomLevelAccelerationStructure&>(*Instance.BottomLevelPtr);
            vk::AccelerationStructureInstanceKHR VkInstance{};
            const auto Flags = Instance.Flags == RHIAccelerationStructureInstanceFlags::DisableTriangleCulling
                ? vk::GeometryInstanceFlagsKHR{vk::GeometryInstanceFlagBitsKHR::eTriangleFacingCullDisable}
                : vk::GeometryInstanceFlagsKHR{};
            VkInstance.setTransform(ToVkInstanceTransform(Instance.Transform))
                .setInstanceCustomIndex(Instance.CustomIndex)
                .setMask(Instance.Mask)
                .setInstanceShaderBindingTableRecordOffset(Instance.HitGroupIndex)
                .setFlags(Flags)
                .setAccelerationStructureReference(GetAccelerationStructureAddress(*m_Device, Blas.GetAccelerationStructure()));
            VkInstances.push_back(VkInstance);
        }
        if (auto R = m_InstanceBuffer->Upload(VkInstances.data(), VkInstances.size() * sizeof(vk::AccelerationStructureInstanceKHR)); !R)
            return std::unexpected(R.error());
        const vk::DeviceAddress InstanceAddress = m_Device->getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = m_InstanceBuffer->Get()});
        vk::AccelerationStructureGeometryInstancesDataKHR InstanceData{
            .arrayOfPointers = vk::False,
            .data = vk::DeviceOrHostAddressConstKHR{.deviceAddress = InstanceAddress},
        };
        vk::AccelerationStructureGeometryKHR Geometry{
            .geometryType = vk::GeometryTypeKHR::eInstances,
            .geometry = vk::AccelerationStructureGeometryDataKHR{.instances = InstanceData},
        };
        const bool bCanUpdate = Mode != RHITopLevelAccelerationStructureBuildMode::Build && m_LastInstanceCount == Instances.size() &&
            (static_cast<Uint32>(m_BuildFlags) & static_cast<Uint32>(RHIAccelerationStructureBuildFlags::AllowUpdate)) != 0;
        vk::AccelerationStructureBuildGeometryInfoKHR BuildGeometryCI{
            .type = vk::AccelerationStructureTypeKHR::eTopLevel,
            .flags = ToVkBuildFlags(m_BuildFlags),
            .mode = bCanUpdate ? vk::BuildAccelerationStructureModeKHR::eUpdate : vk::BuildAccelerationStructureModeKHR::eBuild,
            .srcAccelerationStructure = bCanUpdate ? m_Native->Get() : nullptr,
            .dstAccelerationStructure = m_Native->Get(),
            .geometryCount = 1,
            .pGeometries = &Geometry,
            .scratchData = vk::DeviceOrHostAddressKHR{.deviceAddress = m_Device->getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = m_ScratchBuffer->Get()})},
        };
        const vk::AccelerationStructureBuildRangeInfoKHR Range{.primitiveCount = static_cast<Uint32>(Instances.size())};
        const vk::AccelerationStructureBuildRangeInfoKHR* RangePtr = &Range;
        CmdBuf.buildAccelerationStructuresKHR(BuildGeometryCI, {RangePtr});
        m_LastInstanceCount = static_cast<Uint32>(Instances.size());
        return {};
    }

  private:
    [[nodiscard]] auto GrowTo(Uint32 RequiredCapacity) -> std::expected<void, ErrorMessage> {
        const Uint32 NewCapacity = (std::max)(RequiredCapacity, m_InstanceCapacity * 2);
        vk::AccelerationStructureGeometryInstancesDataKHR InstanceData{
            .arrayOfPointers = vk::False,
            .data = vk::DeviceOrHostAddressConstKHR{.deviceAddress = 0},
        };
        vk::AccelerationStructureGeometryKHR Geometry{
            .geometryType = vk::GeometryTypeKHR::eInstances,
            .geometry = vk::AccelerationStructureGeometryDataKHR{.instances = InstanceData},
        };
        vk::AccelerationStructureBuildGeometryInfoKHR BuildGeometryCI{
            .type = vk::AccelerationStructureTypeKHR::eTopLevel,
            .flags = ToVkBuildFlags(m_BuildFlags),
            .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
            .geometryCount = 1,
            .pGeometries = &Geometry,
        };
        const std::array<Uint32, 1> PrimitiveCounts{NewCapacity};
        const auto Sizes = m_Device->getAccelerationStructureBuildSizesKHR(
            vk::AccelerationStructureBuildTypeKHR::eDevice, BuildGeometryCI, PrimitiveCounts);
        auto Native = VulkanNativeAccelerationStructure::Create(
            *m_Device, m_Allocator, vk::AccelerationStructureTypeKHR::eTopLevel, Sizes.accelerationStructureSize);
        if (!Native)
            return std::unexpected(Native.error().Append("Failed to grow TLAS storage"));
        auto InstanceBuffer = VulkanVmaBuffer::CreateHostVisible(
            static_cast<Uint64>(NewCapacity) * sizeof(vk::AccelerationStructureInstanceKHR),
            vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR,
            m_Allocator);
        if (!InstanceBuffer)
            return std::unexpected(InstanceBuffer.error().Append("Failed to grow TLAS instance buffer"));
        auto ScratchBuffer = VulkanVmaBuffer::Create(Sizes.buildScratchSize, vk::BufferUsageFlagBits::eStorageBuffer, m_Allocator);
        if (!ScratchBuffer)
            return std::unexpected(ScratchBuffer.error().Append("Failed to grow TLAS scratch buffer"));

        auto OldNative = std::move(m_Native);
        auto OldInstanceBuffer = std::move(m_InstanceBuffer);
        auto OldScratchBuffer = std::move(m_ScratchBuffer);
        m_Native = std::move(*Native);
        m_InstanceBuffer = std::move(*InstanceBuffer);
        m_ScratchBuffer = std::move(*ScratchBuffer);
        m_InstanceCapacity = NewCapacity;
        m_LastInstanceCount = 0;
        m_DeletionQueue->Enqueue(GetLastUsageToken(), [Native = std::move(OldNative),
                                                        InstanceBuffer = std::move(OldInstanceBuffer),
                                                        ScratchBuffer = std::move(OldScratchBuffer)]() {});
        return {};
    }

    vk::raii::Device*                          m_Device = nullptr;
    VmaAllocator                               m_Allocator = nullptr;
    SPtr<VulkanNativeAccelerationStructure>          m_Native = nullptr;
    SPtr<VulkanVmaBuffer>                            m_InstanceBuffer = nullptr;
    SPtr<VulkanVmaBuffer>                            m_ScratchBuffer = nullptr;
    Uint32                                     m_InstanceCapacity = 0;
    Uint32                                     m_LastInstanceCount = 0;
    RHIAccelerationStructureBuildFlags       m_BuildFlags = RHIAccelerationStructureBuildFlags::None;
    VulkanDeletionQueue*                             m_DeletionQueue = nullptr;
};

} // namespace SoulEngine
