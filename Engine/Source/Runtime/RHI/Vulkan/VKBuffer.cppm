module;

#include <vk_mem_alloc.h>

export module Vulkan:Buffer;

import Core;
import vulkan;
import RHI;
import std;

import :Types;
import :ImmediateContext;
import :DeletionQueue;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

// ═════════════════════════════════════════════════════════════════════════════
// HostBuffer — mappable staging buffer
// ═════════════════════════════════════════════════════════════════════════════

/// Internal mappable buffer for staging uploads.
/// Created via static Create(). Move-only.
class HostBuffer {
  public:
    HostBuffer() = default;

    [[nodiscard]] static auto Create(Uint64 Size, vk::BufferUsageFlags Usage, vk::Device Dev, VmaAllocator Alloc)
        -> std::expected<HostBuffer, ErrorMessage> {
        HostBuffer Buf;
        Buf.m_Allocator = Alloc;
        Buf.m_Device    = Dev;
        Buf.m_Size      = Size;

        vk::BufferCreateInfo BufCI{
            .size        = Size,
            .usage       = Usage,
            .sharingMode = vk::SharingMode::eExclusive,
        };

        VmaAllocationCreateInfo AllocInfo{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };

        if (vmaCreateBuffer(Alloc,
                            reinterpret_cast<VkBufferCreateInfo*>(&BufCI),
                            &AllocInfo,
                            reinterpret_cast<VkBuffer*>(&Buf.m_Buffer),
                            &Buf.m_Allocation,
                            nullptr) != VK_SUCCESS) {
            return std::unexpected(ErrorMessage("Failed to create HostBuffer via VMA"));
        }

        return Buf;
    }

    ~HostBuffer() {
        Destroy();
    }

    HostBuffer(HostBuffer&& Other) noexcept
        : m_Allocator(Other.m_Allocator),
          m_Device(Other.m_Device),
          m_Buffer(Other.m_Buffer),
          m_Allocation(Other.m_Allocation),
          m_Size(Other.m_Size) {
        Other.m_Buffer     = nullptr;
        Other.m_Allocation = nullptr;
        Other.m_Size       = 0;
    }

    auto operator=(HostBuffer&& Other) noexcept -> HostBuffer& {
        if (this != &Other) {
            Destroy();
            m_Allocator        = Other.m_Allocator;
            m_Device           = Other.m_Device;
            m_Buffer           = Other.m_Buffer;
            m_Allocation       = Other.m_Allocation;
            m_Size             = Other.m_Size;
            Other.m_Buffer     = nullptr;
            Other.m_Allocation = nullptr;
            Other.m_Size       = 0;
        }
        return *this;
    }

    HostBuffer(const HostBuffer&)     = delete;
    auto operator=(const HostBuffer&) = delete;

    [[nodiscard]] auto Get() const -> vk::Buffer {
        return m_Buffer;
    }
    [[nodiscard]] auto GetSize() const -> Uint64 {
        return m_Size;
    }

    /// Upload host data to the buffer at Offset.
    /// One-shot: map -> memcpy -> unmap.
    [[nodiscard]] auto Upload(const void* Data, Uint64 Size, Uint64 Offset = 0) -> std::expected<void, ErrorMessage> {
        if (Offset + Size > m_Size)
            return std::unexpected(ErrorMessage(Core::Format(
                "HostBuffer upload exceeds size (offset {} + size {} > capacity {})", Offset, Size, m_Size)));
        if (Size == 0)
            return {};

        auto Result = vmaCopyMemoryToAllocation(
            m_Allocator, Data, m_Allocation, static_cast<VkDeviceSize>(Offset), static_cast<VkDeviceSize>(Size));
        if (Result != VK_SUCCESS)
            return std::unexpected(ErrorMessage("vmaCopyMemoryToAllocation failed in HostBuffer::Upload"));
        return {};
    }

    /// Defer destruction to DeferredDeletionQueue at the given timeline Value.
    /// After this call the HostBuffer is hollowed out (m_Allocation = nullptr)
    /// so its destructor is a no-op. The lambda captures VMA handles by value.
    auto DeferredDelete(DeferredDeletionQueue& Queue, Uint64 Value) -> void {
        auto Alloc       = m_Allocator;
        auto Buf         = static_cast<VkBuffer>(m_Buffer);
        auto AllocHandle = m_Allocation;
        Queue.Enqueue(Value, [Alloc, Buf, AllocHandle]() {
            if (AllocHandle)
                vmaDestroyBuffer(Alloc, Buf, AllocHandle);
        });
        m_Allocation = nullptr;
        m_Buffer     = nullptr;
        m_Size       = 0;
    }

  private:
    auto Destroy() -> void {
        if (m_Allocation) {
            vmaDestroyBuffer(m_Allocator, static_cast<VkBuffer>(m_Buffer), m_Allocation);
        }
    }

    VmaAllocator  m_Allocator  = nullptr;
    vk::Device    m_Device     = nullptr;
    vk::Buffer    m_Buffer     = nullptr;
    VmaAllocation m_Allocation = nullptr;
    Uint64        m_Size       = 0;
};

// ═════════════════════════════════════════════════════════════════════════════
// DeviceBuffer — device-local buffer
// ═════════════════════════════════════════════════════════════════════════════

/// Internal device-local buffer for GPU-only access.
/// Move-only. Data transferred via CopyFrom with a HostBuffer staging source.
class DeviceBuffer {
  public:
    DeviceBuffer() = default;

    [[nodiscard]] static auto Create(Uint64 Size, vk::BufferUsageFlags Usage, vk::Device Dev, VmaAllocator Alloc)
        -> std::expected<DeviceBuffer, ErrorMessage> {
        DeviceBuffer Buf;
        Buf.m_Allocator = Alloc;
        Buf.m_Device    = Dev;
        Buf.m_Size      = Size;

        vk::BufferCreateInfo BufCI{
            .size        = Size,
            .usage       = Usage,
            .sharingMode = vk::SharingMode::eExclusive,
        };

        VmaAllocationCreateInfo AllocInfo{
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        };

        if (vmaCreateBuffer(Alloc,
                            reinterpret_cast<VkBufferCreateInfo*>(&BufCI),
                            &AllocInfo,
                            reinterpret_cast<VkBuffer*>(&Buf.m_Buffer),
                            &Buf.m_Allocation,
                            nullptr) != VK_SUCCESS) {
            return std::unexpected(ErrorMessage("Failed to create DeviceBuffer via VMA"));
        }

        return Buf;
    }

    ~DeviceBuffer() {
        Destroy();
    }

    DeviceBuffer(DeviceBuffer&& Other) noexcept
        : m_Allocator(Other.m_Allocator),
          m_Device(Other.m_Device),
          m_Buffer(Other.m_Buffer),
          m_Allocation(Other.m_Allocation),
          m_Size(Other.m_Size) {
        Other.m_Buffer     = nullptr;
        Other.m_Allocation = nullptr;
        Other.m_Size       = 0;
    }

    auto operator=(DeviceBuffer&& Other) noexcept -> DeviceBuffer& {
        if (this != &Other) {
            Destroy();
            m_Allocator        = Other.m_Allocator;
            m_Device           = Other.m_Device;
            m_Buffer           = Other.m_Buffer;
            m_Allocation       = Other.m_Allocation;
            m_Size             = Other.m_Size;
            Other.m_Buffer     = nullptr;
            Other.m_Allocation = nullptr;
            Other.m_Size       = 0;
        }
        return *this;
    }

    DeviceBuffer(const DeviceBuffer&)   = delete;
    auto operator=(const DeviceBuffer&) = delete;

    [[nodiscard]] auto Get() const -> vk::Buffer {
        return m_Buffer;
    }
    [[nodiscard]] auto GetSize() const -> Uint64 {
        return m_Size;
    }

    /// Copy full contents from a HostBuffer staging source via ImmediateContext.
    /// Copies min(SrcSize, this->Size) bytes. Signals SignalSema on completion.
    [[nodiscard]] auto CopyFrom(HostBuffer& Src, ImmediateContext& Ctx, const vk::SemaphoreSubmitInfo& SignalSema)
        -> std::expected<void, ErrorMessage> {
        Uint64 CopySize = std::min(Src.GetSize(), m_Size);

        auto CopyResult = Ctx.SubmitTransfer(
            [&](const vk::raii::CommandBuffer& CmdBuf) {
                vk::BufferCopy Region{.srcOffset = 0, .dstOffset = 0, .size = CopySize};
                CmdBuf.copyBuffer(Src.Get(), m_Buffer, {Region});
            },
            SignalSema);

        if (!CopyResult)
            return std::unexpected(CopyResult.error().Append("DeviceBuffer::CopyFrom failed"));
        return {};
    }

  private:
    auto Destroy() -> void {
        if (m_Allocation)
            vmaDestroyBuffer(m_Allocator, static_cast<VkBuffer>(m_Buffer), m_Allocation);
    }

    VmaAllocator  m_Allocator  = nullptr;
    vk::Device    m_Device     = nullptr;
    vk::Buffer    m_Buffer     = nullptr;
    VmaAllocation m_Allocation = nullptr;
    Uint64        m_Size       = 0;
};

// ═════════════════════════════════════════════════════════════════════════════
// Vulkan::VertexBuffer — typed vertex buffer
// ═════════════════════════════════════════════════════════════════════════════

class VertexBuffer final : public RHI::VertexBuffer {
  public:
    // Public for std::make_shared compatibility per ADR 02.
    // All callers should use Create() instead.
    VertexBuffer(DeviceBuffer&& Buf, Uint32 Stride, Uint64 VertexCount)
        : m_Buffer(std::move(Buf)), m_Stride(Stride), m_VertexCount(VertexCount) {}

    /// Static factory: creates staging buffer, uploads data, copies to
    /// device-local buffer via ImmediateContext, and defers staging destruction
    /// to DeferredDeletionQueue.
    [[nodiscard]] static auto Create(const RHI::VertexBufferDesc& Desc,
                                     VmaAllocator                 Alloc,
                                     vk::Device                   Dev,
                                     ImmediateContext&            ImmCtx,
                                     DeferredDeletionQueue&       DelQueue)
        -> std::expected<SPtr<VertexBuffer>, ErrorMessage> {
        Uint64 Size  = Desc.VertexCount * Desc.Stride;
        Uint64 VCnt  = Desc.VertexCount;
        Uint32 Strid = Desc.Stride;

        // ── Staging buffer ──────────────────────────────────────────────
        HostBuffer Staging;
        auto       StagingRes = HostBuffer::Create(Size, vk::BufferUsageFlagBits::eTransferSrc, Dev, Alloc);
        if (!StagingRes)
            return std::unexpected(StagingRes.error().Append("VertexBuffer::Create: staging creation failed"));
        Staging = std::move(*StagingRes);

        if (auto R = Staging.Upload(Desc.Data, Size); !R)
            return std::unexpected(R.error().Append("VertexBuffer::Create: staging upload failed"));

        // ── Device buffer ────────────────────────────────────────────────
        DeviceBuffer DevBuf;
        auto         DevRes = DeviceBuffer::Create(
            Size, vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst, Dev, Alloc);
        if (!DevRes)
            return std::unexpected(DevRes.error().Append("VertexBuffer::Create: device buffer creation failed"));
        DevBuf = std::move(*DevRes);

        // ── Copy staging -> device with timeline signal ────────────────
        uint64_t SignalValue = DelQueue.NextValue();
        auto     SignalSema  = DelQueue.MakeSignalSubmitInfo(SignalValue, vk::PipelineStageFlagBits2::eTransfer);
        if (auto R = DevBuf.CopyFrom(Staging, ImmCtx, SignalSema); !R)
            return std::unexpected(R.error().Append("VertexBuffer::Create: staging copy failed"));

        // ── Defer staging destruction ──────────────────────────────────
        Staging.DeferredDelete(DelQueue, SignalValue);

        return std::make_shared<VertexBuffer>(std::move(DevBuf), Strid, VCnt);
    }

    [[nodiscard]] auto GetVkBuffer() const -> vk::Buffer {
        return m_Buffer.Get();
    }
    [[nodiscard]] auto GetStride() const -> Uint32 {
        return m_Stride;
    }
    [[nodiscard]] auto GetVertexCount() const -> Uint64 {
        return m_VertexCount;
    }

    VertexBuffer(const VertexBuffer&)                    = delete;
    auto operator=(const VertexBuffer&) -> VertexBuffer& = delete;
    VertexBuffer(VertexBuffer&&)                         = delete;
    auto operator=(VertexBuffer&&) -> VertexBuffer&      = delete;

  private:
    DeviceBuffer m_Buffer;
    Uint32       m_Stride      = 0;
    Uint64       m_VertexCount = 0;
};

// ═════════════════════════════════════════════════════════════════════════════
// Vulkan::IndexBuffer — typed index buffer
// ═════════════════════════════════════════════════════════════════════════════

class IndexBuffer final : public RHI::IndexBuffer {
  public:
    // Public for std::make_shared compatibility per ADR 02.
    // All callers should use Create() instead.
    IndexBuffer(DeviceBuffer&& Buf, Uint64 IndexCount) : m_Buffer(std::move(Buf)), m_IndexCount(IndexCount) {}

    /// Static factory: same pattern as VertexBuffer::Create.
    /// Index type is hardcoded to uint32 (eUint32).  uint16 is not supported.
    [[nodiscard]] static auto Create(const RHI::IndexBufferDesc& Desc,
                                     VmaAllocator                Alloc,
                                     vk::Device                  Dev,
                                     ImmediateContext&           ImmCtx,
                                     DeferredDeletionQueue&      DelQueue)
        -> std::expected<SPtr<IndexBuffer>, ErrorMessage> {
        Uint64 IndexCount = Desc.IndexCount;
        Uint64 Size       = IndexCount * 4ULL;

        HostBuffer Staging;
        auto       StagingRes = HostBuffer::Create(Size, vk::BufferUsageFlagBits::eTransferSrc, Dev, Alloc);
        if (!StagingRes)
            return std::unexpected(StagingRes.error().Append("IndexBuffer::Create: staging creation failed"));
        Staging = std::move(*StagingRes);

        if (auto R = Staging.Upload(Desc.Data, Size); !R)
            return std::unexpected(R.error().Append("IndexBuffer::Create: staging upload failed"));

        DeviceBuffer DevBuf;
        auto         DevRes = DeviceBuffer::Create(
            Size, vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst, Dev, Alloc);
        if (!DevRes)
            return std::unexpected(DevRes.error().Append("IndexBuffer::Create: device buffer creation failed"));
        DevBuf = std::move(*DevRes);

        uint64_t SignalValue = DelQueue.NextValue();
        auto     SignalSema  = DelQueue.MakeSignalSubmitInfo(SignalValue, vk::PipelineStageFlagBits2::eTransfer);
        if (auto R = DevBuf.CopyFrom(Staging, ImmCtx, SignalSema); !R)
            return std::unexpected(R.error().Append("IndexBuffer::Create: staging copy failed"));

        Staging.DeferredDelete(DelQueue, SignalValue);

        return std::make_shared<IndexBuffer>(std::move(DevBuf), IndexCount);
    }

    [[nodiscard]] auto GetVkBuffer() const -> vk::Buffer {
        return m_Buffer.Get();
    }
    [[nodiscard]] auto GetIndexCount() const -> Uint64 {
        return m_IndexCount;
    }

    IndexBuffer(const IndexBuffer&)                    = delete;
    auto operator=(const IndexBuffer&) -> IndexBuffer& = delete;
    IndexBuffer(IndexBuffer&&)                         = delete;
    auto operator=(IndexBuffer&&) -> IndexBuffer&      = delete;

  private:
    DeviceBuffer m_Buffer;
    Uint64       m_IndexCount = 0;
};

// ═════════════════════════════════════════════════════════════════════════════
// Vulkan::UniformBuffer — single mappable uniform buffer
// ═════════════════════════════════════════════════════════════════════════════

/// Mappable uniform buffer backed by a single HostBuffer.
/// No awareness of FramesInFlight — Write() writes to this one buffer.
///
/// Per-frame duplication is the caller's concern (RenderDevice creates N
/// copies, one per FrameContext slot).
class UniformBuffer final : public RHI::ConstantBuffer {
  public:
    // Public for std::make_shared compatibility per ADR 02.
    // All callers should use Create() instead.
    UniformBuffer(HostBuffer&& Buf, Uint64 Size) : m_Buffer(std::move(Buf)), m_Size(Size) {}

    /// Static factory: creates a mappable UniformBuffer.
    /// Returns error if VMA allocation fails.
    [[nodiscard]] static auto Create(const RHI::ConstantBufferDesc& Desc, vk::Device Dev, VmaAllocator Alloc)
        -> std::expected<SPtr<UniformBuffer>, ErrorMessage> {
        auto HostRes = HostBuffer::Create(Desc.Size, vk::BufferUsageFlagBits::eUniformBuffer, Dev, Alloc);
        if (!HostRes)
            return std::unexpected(HostRes.error().Append("UniformBuffer::Create: HostBuffer creation failed"));
        return std::make_shared<UniformBuffer>(std::move(*HostRes), Desc.Size);
    }

    UniformBuffer(const UniformBuffer&)                    = delete;
    auto operator=(const UniformBuffer&) -> UniformBuffer& = delete;
    UniformBuffer(UniformBuffer&&)                         = delete;
    auto operator=(UniformBuffer&&) -> UniformBuffer&      = delete;

    /// Upload new data to the buffer.
    [[nodiscard]] auto Write(const void* Data, Uint64 Size) -> std::expected<void, ErrorMessage> override {
        return m_Buffer.Upload(Data, Size, 0);
    }

    [[nodiscard]] auto GetSize() const -> Uint64 override {
        return m_Size;
    }

    [[nodiscard]] auto GetVkBuffer() const -> vk::Buffer {
        return m_Buffer.Get();
    }

  private:
    HostBuffer m_Buffer;
    Uint64     m_Size = 0;
};

} // namespace SoulEngine::RHI::Vulkan
