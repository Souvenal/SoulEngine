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

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

namespace {

[[nodiscard]] auto ToVkBuildFlags(RHI::AccelerationStructureBuildFlags Flags) -> vk::BuildAccelerationStructureFlagsKHR {
    auto Result = vk::BuildAccelerationStructureFlagsKHR{};
    const auto Raw = static_cast<Uint32>(Flags);
    if ((Raw & static_cast<Uint32>(RHI::AccelerationStructureBuildFlags::PreferFastTrace)) != 0)
        Result |= vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
    if ((Raw & static_cast<Uint32>(RHI::AccelerationStructureBuildFlags::PreferFastBuild)) != 0)
        Result |= vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild;
    if ((Raw & static_cast<Uint32>(RHI::AccelerationStructureBuildFlags::AllowUpdate)) != 0)
        Result |= vk::BuildAccelerationStructureFlagBitsKHR::eAllowUpdate;
    return Result;
}

[[nodiscard]] auto ToVkGeometryFlags(RHI::AccelerationStructureGeometryFlags Flags) -> vk::GeometryFlagsKHR {
    auto Result = vk::GeometryFlagsKHR{};
    if ((static_cast<Uint32>(Flags) & static_cast<Uint32>(RHI::AccelerationStructureGeometryFlags::Opaque)) != 0)
        Result |= vk::GeometryFlagBitsKHR::eOpaque;
    return Result;
}

[[nodiscard]] auto ToVkInstanceTransform(const RHI::RowMajorTransform3x4& Transform) -> vk::TransformMatrixKHR {
    return vk::TransformMatrixKHR{std::array<std::array<Float32, 4>, 3>{
        std::array<Float32, 4>{Transform.M00, Transform.M01, Transform.M02, Transform.M03},
        std::array<Float32, 4>{Transform.M10, Transform.M11, Transform.M12, Transform.M13},
        std::array<Float32, 4>{Transform.M20, Transform.M21, Transform.M22, Transform.M23},
    }};
}

[[nodiscard]] auto ToVkVertexFormat(RHI::Format FormatValue) -> std::expected<vk::Format, ErrorMessage> {
    switch (FormatValue) {
    case RHI::Format::R32G32B32_SFLOAT:
        return vk::Format::eR32G32B32Sfloat;
    default:
        return std::unexpected(ErrorMessage("Acceleration-structure geometry requires R32G32B32_SFLOAT positions"));
    }
}

class VmaBuffer final {
  public:
    VmaBuffer() = default;
    ~VmaBuffer() {
        if (m_Allocation)
            vmaDestroyBuffer(m_Allocator, static_cast<VkBuffer>(m_Buffer), m_Allocation);
    }

    VmaBuffer(const VmaBuffer&)                    = delete;
    auto operator=(const VmaBuffer&) -> VmaBuffer& = delete;
    VmaBuffer(VmaBuffer&&)                         = delete;
    auto operator=(VmaBuffer&&) -> VmaBuffer&      = delete;

    [[nodiscard]] static auto Create(Uint64 Size, vk::BufferUsageFlags Usage, VmaAllocator Allocator)
        -> std::expected<SPtr<VmaBuffer>, ErrorMessage> {
        if (Size == 0)
            return std::unexpected(ErrorMessage("Acceleration-structure buffer size must be non-zero"));

        auto Result       = std::make_shared<VmaBuffer>();
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
        -> std::expected<SPtr<VmaBuffer>, ErrorMessage> {
        if (Size == 0)
            return std::unexpected(ErrorMessage("Host-visible acceleration-structure buffer size must be non-zero"));

        auto Result       = std::make_shared<VmaBuffer>();
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

class NativeAccelerationStructure final {
  public:
    NativeAccelerationStructure() = default;

    [[nodiscard]] static auto Create(vk::raii::Device&                 Device,
                                     VmaAllocator                       Allocator,
                                     vk::AccelerationStructureTypeKHR  Type,
                                     Uint64                             Size)
        -> std::expected<SPtr<NativeAccelerationStructure>, ErrorMessage> {
        auto Storage = VmaBuffer::Create(Size, vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR, Allocator);
        if (!Storage)
            return std::unexpected(Storage.error());

        vk::AccelerationStructureCreateInfoKHR AccelerationStructureCI{
            .buffer = (*Storage)->Get(),
            .size   = Size,
            .type   = Type,
        };
        auto [Result, AccelerationStructure] = Device.createAccelerationStructureKHR(AccelerationStructureCI, nullptr);
        if (Result != vk::Result::eSuccess) {
            return std::unexpected(ErrorMessage(Core::Format(
                "Failed to create Vulkan acceleration structure: {}", vk::to_string(Result))));
        }

        auto Native = std::make_shared<NativeAccelerationStructure>();
        Native->m_Storage = std::move(*Storage);
        Native->m_AccelerationStructure = std::make_shared<vk::raii::AccelerationStructureKHR>(std::move(AccelerationStructure));
        return Native;
    }

    [[nodiscard]] auto Get() const -> vk::AccelerationStructureKHR {
        return *(*m_AccelerationStructure);
    }

  private:
    SPtr<VmaBuffer>                              m_Storage = nullptr;
    SPtr<vk::raii::AccelerationStructureKHR>     m_AccelerationStructure = nullptr;
};

[[nodiscard]] auto GetAccelerationStructureAddress(vk::raii::Device& Device, vk::AccelerationStructureKHR Structure)
    -> vk::DeviceAddress {
    return Device.getAccelerationStructureAddressKHR(
        vk::AccelerationStructureDeviceAddressInfoKHR{.accelerationStructure = Structure});
}

} // namespace

/// Vulkan GPU payload for reusable object-space triangle geometry.
class BottomLevelAccelerationStructure final : public RHI::BottomLevelAccelerationStructure {
  public:
    BottomLevelAccelerationStructure() = default;

    ~BottomLevelAccelerationStructure() override {
        if (m_DeletionQueue)
            m_DeletionQueue->Enqueue(GetLastUsageToken(), [Native = m_Native]() {});
    }

    BottomLevelAccelerationStructure(const BottomLevelAccelerationStructure&)                    = delete;
    auto operator=(const BottomLevelAccelerationStructure&) -> BottomLevelAccelerationStructure& = delete;

    [[nodiscard]] static auto Create(vk::raii::Device&                                   Device,
                                     VmaAllocator                                         Allocator,
                                     ImmediateContext&                                    BuildContext,
                                     TransferCompletionQueue&                             CompletionQueue,
                                     DeletionQueue&                                       DeletionQueueRef,
                                     const RHI::BottomLevelAccelerationStructureDesc&     Desc)
        -> std::expected<UPtr<RHI::BottomLevelAccelerationStructure>, ErrorMessage> {
        if (!Capability::Get().GetRayTracingSupport().Available)
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
            if (GeometryDesc.IndexType != RHI::AccelerationStructureIndexType::Uint32)
                return std::unexpected(ErrorMessage("BLAS currently supports only Uint32 index buffers"));

            auto VertexFormat = ToVkVertexFormat(GeometryDesc.VertexFormat);
            if (!VertexFormat)
                return std::unexpected(VertexFormat.error());
            auto& VertexBuffer = static_cast<const Vulkan::VertexBuffer&>(*GeometryDesc.VertexBufferPtr);
            auto& IndexBuffer = static_cast<const Vulkan::IndexBuffer&>(*GeometryDesc.IndexBufferPtr);
            const vk::DeviceAddress VertexAddress =
                Device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = VertexBuffer.GetVkBuffer()});
            const vk::DeviceAddress IndexAddress =
                Device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = IndexBuffer.GetVkBuffer()});
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
        auto Native = NativeAccelerationStructure::Create(
            Device, Allocator, vk::AccelerationStructureTypeKHR::eBottomLevel, Sizes.accelerationStructureSize);
        if (!Native)
            return std::unexpected(Native.error().Append("Failed to allocate BLAS storage"));
        auto Scratch = VmaBuffer::Create(
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

        auto Result = std::make_unique<BottomLevelAccelerationStructure>();
        Result->m_Native = std::move(*Native);
        Result->m_DeletionQueue = &DeletionQueueRef;
        return UPtr<RHI::BottomLevelAccelerationStructure>{std::move(Result)};
    }

    [[nodiscard]] auto GetAccelerationStructure() const -> vk::AccelerationStructureKHR {
        return m_Native->Get();
    }

  private:
    SPtr<NativeAccelerationStructure> m_Native = nullptr;
    DeletionQueue*                    m_DeletionQueue = nullptr;
};

/// Vulkan persistent TLAS allocation. Instance uploads and builds are recorded by the command encoder.
class TopLevelAccelerationStructure final : public RHI::TopLevelAccelerationStructure {
  public:
    TopLevelAccelerationStructure() = default;

    ~TopLevelAccelerationStructure() override {
        if (m_DeletionQueue)
            m_DeletionQueue->Enqueue(GetLastUsageToken(), [Native = m_Native]() {});
    }

    TopLevelAccelerationStructure(const TopLevelAccelerationStructure&)                    = delete;
    auto operator=(const TopLevelAccelerationStructure&) -> TopLevelAccelerationStructure& = delete;

    [[nodiscard]] static auto Create(vk::raii::Device&                                Device,
                                     VmaAllocator                                      Allocator,
                                     DeletionQueue&                                    DeletionQueueRef,
                                     const RHI::TopLevelAccelerationStructureDesc&     Desc)
        -> std::expected<UPtr<RHI::TopLevelAccelerationStructure>, ErrorMessage> {
        if (!Capability::Get().GetRayTracingSupport().Available)
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
        auto Native = NativeAccelerationStructure::Create(
            Device, Allocator, vk::AccelerationStructureTypeKHR::eTopLevel, Sizes.accelerationStructureSize);
        if (!Native)
            return std::unexpected(Native.error().Append("Failed to allocate TLAS storage"));

        auto InstanceBuffer = VmaBuffer::CreateHostVisible(
            static_cast<Uint64>(Desc.InitialInstanceCapacity) * sizeof(vk::AccelerationStructureInstanceKHR),
            vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR,
            Allocator);
        if (!InstanceBuffer)
            return std::unexpected(InstanceBuffer.error().Append("Failed to allocate TLAS instance buffer"));
        auto ScratchBuffer = VmaBuffer::Create(Sizes.buildScratchSize, vk::BufferUsageFlagBits::eStorageBuffer, Allocator);
        if (!ScratchBuffer)
            return std::unexpected(ScratchBuffer.error().Append("Failed to allocate TLAS scratch buffer"));

        auto Result = std::make_unique<TopLevelAccelerationStructure>();
        Result->m_Device = &Device;
        Result->m_Allocator = Allocator;
        Result->m_Native = std::move(*Native);
        Result->m_InstanceBuffer = std::move(*InstanceBuffer);
        Result->m_ScratchBuffer = std::move(*ScratchBuffer);
        Result->m_InstanceCapacity = Desc.InitialInstanceCapacity;
        Result->m_BuildFlags = Desc.BuildFlags;
        Result->m_DeletionQueue = &DeletionQueueRef;
        return UPtr<RHI::TopLevelAccelerationStructure>{std::move(Result)};
    }

    [[nodiscard]] auto GetInstanceCapacity() const -> Uint32 {
        return m_InstanceCapacity;
    }

    [[nodiscard]] auto GetAccelerationStructure() const -> vk::AccelerationStructureKHR {
        return m_Native->Get();
    }

    [[nodiscard]] auto RecordBuild(const vk::raii::CommandBuffer& CmdBuf,
                                   std::span<const RHI::AccelerationStructureInstance> Instances,
                                   RHI::TopLevelAccelerationStructureBuildMode Mode) -> std::expected<void, ErrorMessage> {
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
            const auto& Blas = static_cast<const Vulkan::BottomLevelAccelerationStructure&>(*Instance.BottomLevelPtr);
            vk::AccelerationStructureInstanceKHR VkInstance{};
            const auto Flags = Instance.Flags == RHI::AccelerationStructureInstanceFlags::DisableTriangleCulling
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
        const bool bCanUpdate = Mode != RHI::TopLevelAccelerationStructureBuildMode::Build && m_LastInstanceCount == Instances.size() &&
            (static_cast<Uint32>(m_BuildFlags) & static_cast<Uint32>(RHI::AccelerationStructureBuildFlags::AllowUpdate)) != 0;
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
        auto Native = NativeAccelerationStructure::Create(
            *m_Device, m_Allocator, vk::AccelerationStructureTypeKHR::eTopLevel, Sizes.accelerationStructureSize);
        if (!Native)
            return std::unexpected(Native.error().Append("Failed to grow TLAS storage"));
        auto InstanceBuffer = VmaBuffer::CreateHostVisible(
            static_cast<Uint64>(NewCapacity) * sizeof(vk::AccelerationStructureInstanceKHR),
            vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR,
            m_Allocator);
        if (!InstanceBuffer)
            return std::unexpected(InstanceBuffer.error().Append("Failed to grow TLAS instance buffer"));
        auto ScratchBuffer = VmaBuffer::Create(Sizes.buildScratchSize, vk::BufferUsageFlagBits::eStorageBuffer, m_Allocator);
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
    SPtr<NativeAccelerationStructure>          m_Native = nullptr;
    SPtr<VmaBuffer>                            m_InstanceBuffer = nullptr;
    SPtr<VmaBuffer>                            m_ScratchBuffer = nullptr;
    Uint32                                     m_InstanceCapacity = 0;
    Uint32                                     m_LastInstanceCount = 0;
    RHI::AccelerationStructureBuildFlags       m_BuildFlags = RHI::AccelerationStructureBuildFlags::None;
    DeletionQueue*                             m_DeletionQueue = nullptr;
};

} // namespace SoulEngine::RHI::Vulkan
