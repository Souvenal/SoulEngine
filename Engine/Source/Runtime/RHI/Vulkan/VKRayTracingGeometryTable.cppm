module;

#include <cstddef>

export module Vulkan:RayTracingGeometryTable;

import Core;
import RHI;
import vulkan;
import std;

import :Buffer;
import :Semaphore;

namespace SoulEngine {

namespace {

constexpr Uint64 kGeometryTableCapacity = 4ULL * 1024ULL * 1024ULL;

struct alignas(16) GeometryTableHeader {
    Uint32 InstanceCount = 0;
    Uint32 GeometryCount = 0;
    Uint32 InstanceOffset = sizeof(GeometryTableHeader);
    Uint32 GeometryOffset = sizeof(GeometryTableHeader);
};

struct alignas(16) InstanceGeometryRecord {
    Uint32 FirstGeometry = 0;
    Uint32 GeometryCount = 0;
    Uint32 MaterialIndex = 0;
    Uint32 Reserved      = 0;
};

struct alignas(16) GeometryAddressRecord {
    Uint64 PositionAddress    = 0;
    Uint64 NormalAddress      = 0;
    Uint64 IndexAddress       = 0;
    Uint32 PositionByteOffset = 0;
    Uint32 NormalByteOffset   = 0;
    Uint32 IndexByteOffset    = 0;
    Uint32 PositionStride     = 0;
    Uint32 NormalStride       = 0;
    Uint32 IndexStride        = 0;
    Uint32 VertexCount        = 0;
    Uint32 IndexCount         = 0;
    Uint32 Reserved0          = 0;
    Uint32 Reserved1          = 0;
};

static_assert(sizeof(GeometryTableHeader) == 16);
static_assert(sizeof(InstanceGeometryRecord) == 16);
static_assert(sizeof(GeometryAddressRecord) == 64);
static_assert(alignof(GeometryAddressRecord) == 16);
static_assert(offsetof(GeometryAddressRecord, PositionAddress) == 0);
static_assert(offsetof(GeometryAddressRecord, NormalAddress) == 8);
static_assert(offsetof(GeometryAddressRecord, IndexAddress) == 16);
static_assert(offsetof(GeometryAddressRecord, PositionByteOffset) == 24);
static_assert(offsetof(GeometryAddressRecord, PositionStride) == 36);
static_assert(offsetof(GeometryAddressRecord, VertexCount) == 48);

[[nodiscard]] auto AlignUp16(Uint64 Value) -> Uint64 {
    return (Value + 15ULL) & ~15ULL;
}

} // namespace

/// Vulkan realization of the VulkanRenderDevice-owned BDA geometry metadata table.
///
/// The table is persistently host-visible because it is rebuilt from the scene
/// snapshot immediately before trace dispatch. RHICommand recording inserts a host
/// write -> ray-tracing shader-read dependency after each update.
class VulkanBdaRayTracingGeometryTable final : public RHIRayTracingGeometryTable {
  public:
    [[nodiscard]] static auto Create(VmaAllocator Allocator, vk::raii::Device& Device, VulkanTimelineSemaphore& Timeline)
        -> std::expected<UPtr<VulkanBdaRayTracingGeometryTable>, ErrorMessage> {
        auto Buffer = VulkanHostBuffer::Create(
            kGeometryTableCapacity, vk::BufferUsageFlagBits::eStorageBuffer, *Device, Allocator);
        if (!Buffer)
            return std::unexpected(Buffer.error().Append("BDA geometry metadata buffer creation failed"));
        return std::make_unique<VulkanBdaRayTracingGeometryTable>(std::move(*Buffer), Device, Timeline);
    }

    VulkanBdaRayTracingGeometryTable(VulkanHostBuffer Buffer, vk::raii::Device& Device, VulkanTimelineSemaphore& Timeline)
        : m_Buffer(std::move(Buffer)), m_Device(&Device), m_Timeline(&Timeline) {}

    [[nodiscard]] auto Update(const RHIRayTracingGeometryTableUpdate& Update) -> std::expected<void, ErrorMessage> {
        // This metadata allocation is shared across frame slots. Wait for the
        // last submitted trace that referenced it before the CPU overwrites it.
        // The token is set only after graphics submission succeeds.
        if (const auto LastUse = GetLastUsageToken(); LastUse.Id != 0) {
            if (auto Result = m_Timeline->Wait(LastUse.Id); !Result)
                return std::unexpected(Result.error().Append("BDA geometry metadata reuse wait failed"));
        }
        const Uint64 InstanceOffset = sizeof(GeometryTableHeader);
        const Uint64 GeometryOffset = AlignUp16(InstanceOffset + sizeof(InstanceGeometryRecord) * Update.Instances.size());
        const Uint64 RequiredSize = GeometryOffset + sizeof(GeometryAddressRecord) * Update.Geometries.size();
        if (RequiredSize > m_Buffer.GetSize()) {
            return std::unexpected(ErrorMessage(Format(
                "BDA geometry metadata needs {} bytes but table capacity is {} bytes", RequiredSize, m_Buffer.GetSize())));
        }
        if (Update.Instances.size() > std::numeric_limits<Uint32>::max() ||
            Update.Geometries.size() > std::numeric_limits<Uint32>::max()) {
            return std::unexpected(ErrorMessage("BDA geometry metadata record count exceeds Uint32 range"));
        }

        std::vector<std::byte> Bytes(static_cast<size_t>(RequiredSize));
        auto* Header = reinterpret_cast<GeometryTableHeader*>(Bytes.data());
        *Header = GeometryTableHeader{
            .InstanceCount = static_cast<Uint32>(Update.Instances.size()),
            .GeometryCount = static_cast<Uint32>(Update.Geometries.size()),
            .InstanceOffset = static_cast<Uint32>(InstanceOffset),
            .GeometryOffset = static_cast<Uint32>(GeometryOffset),
        };

        auto* Instances = reinterpret_cast<InstanceGeometryRecord*>(Bytes.data() + InstanceOffset);
        for (Uint32 Index = 0; Index < Update.Instances.size(); ++Index) {
            const auto& Source = Update.Instances[Index];
            if (Source.GeometryCount == 0 || Source.FirstGeometry > Update.Geometries.size() ||
                Source.GeometryCount > Update.Geometries.size() - Source.FirstGeometry) {
                return std::unexpected(ErrorMessage(Format(
                    "BDA instance record {} has invalid geometry range [{}..{}) for {} geometries",
                    Index,
                    Source.FirstGeometry,
                    static_cast<Uint64>(Source.FirstGeometry) + Source.GeometryCount,
                    Update.Geometries.size())));
            }
            Instances[Index] = InstanceGeometryRecord{
                .FirstGeometry = Source.FirstGeometry,
                .GeometryCount = Source.GeometryCount,
                .MaterialIndex = Source.MaterialIndex,
            };
        }

        auto* Geometries = reinterpret_cast<GeometryAddressRecord*>(Bytes.data() + GeometryOffset);
        for (Uint32 Index = 0; Index < Update.Geometries.size(); ++Index) {
            const auto& Source = Update.Geometries[Index];
            if (!Source.PositionBuffer || !Source.NormalBuffer || !Source.IndexBuffer) {
                return std::unexpected(ErrorMessage(Format("BDA geometry record {} has a null source buffer", Index)));
            }
            if (Source.PositionStride == 0 || Source.NormalStride == 0 || Source.IndexStride != sizeof(Uint32) ||
                Source.VertexCount == 0 || Source.IndexCount == 0) {
                return std::unexpected(ErrorMessage(Format("BDA geometry record {} has an unsupported layout", Index)));
            }

            const auto& Position = static_cast<const VulkanVertexBuffer&>(*Source.PositionBuffer);
            const auto& Normal = static_cast<const VulkanVertexBuffer&>(*Source.NormalBuffer);
            const auto& Indices = static_cast<const VulkanIndexBuffer&>(*Source.IndexBuffer);
            const Uint64 PositionAddress = m_Device->getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = Position.GetVkBuffer()});
            const Uint64 NormalAddress = m_Device->getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = Normal.GetVkBuffer()});
            const Uint64 IndexAddress = m_Device->getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = Indices.GetVkBuffer()});
            if (PositionAddress == 0 || NormalAddress == 0 || IndexAddress == 0) {
                return std::unexpected(ErrorMessage(Format(
                    "BDA geometry record {} has a source buffer without a device address", Index)));
            }

            Geometries[Index] = GeometryAddressRecord{
                .PositionAddress = PositionAddress,
                .NormalAddress = NormalAddress,
                .IndexAddress = IndexAddress,
                .PositionByteOffset = Source.PositionByteOffset,
                .NormalByteOffset = Source.NormalByteOffset,
                .IndexByteOffset = Source.IndexByteOffset,
                .PositionStride = Source.PositionStride,
                .NormalStride = Source.NormalStride,
                .IndexStride = Source.IndexStride,
                .VertexCount = Source.VertexCount,
                .IndexCount = Source.IndexCount,
            };
        }

        if (auto Result = m_Buffer.Upload(Bytes.data(), RequiredSize); !Result)
            return std::unexpected(Result.error().Append("BDA geometry metadata upload failed"));
        m_ByteSize = RequiredSize;
        return {};
    }

    [[nodiscard]] auto GetVkBuffer() const -> vk::Buffer {
        return m_Buffer.Get();
    }

    [[nodiscard]] auto GetByteSize() const -> Uint64 {
        return m_ByteSize;
    }

    [[nodiscard]] auto GetCapacity() const -> Uint64 {
        return m_Buffer.GetSize();
    }

  private:
    VulkanHostBuffer m_Buffer = {};
    vk::raii::Device* m_Device = nullptr;
    VulkanTimelineSemaphore* m_Timeline = nullptr;
    Uint64     m_ByteSize = 0;
};

} // namespace SoulEngine
