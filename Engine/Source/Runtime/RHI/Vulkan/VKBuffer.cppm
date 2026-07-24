module;

#include <vk_mem_alloc.h>

export module Vulkan:Buffer;

import Core;
import vulkan;
import RHI;
import std;

import :Capability;
import :Types;
import :ImmediateContext;
import :TransferCompletionQueue;
import :DeletionQueue;

namespace SoulEngine {

// ═════════════════════════════════════════════════════════════════════════════
// VulkanHostBuffer — mappable staging buffer
// ═════════════════════════════════════════════════════════════════════════════

/// Internal mappable buffer for staging uploads.
/// Created via static Create(). Move-only.
class VulkanHostBuffer {
  public:
    VulkanHostBuffer() = default;

    [[nodiscard]] static auto Create(Uint64 Size, vk::BufferUsageFlags Usage, vk::Device Dev, VmaAllocator Alloc)
        -> std::expected<VulkanHostBuffer, ErrorMessage> {
        VulkanHostBuffer Buf;
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
            return std::unexpected(ErrorMessage("Failed to create VulkanHostBuffer via VMA"));
        }

        return Buf;
    }

    ~VulkanHostBuffer() {
        Destroy();
    }

    VulkanHostBuffer(VulkanHostBuffer&& Other) noexcept
        : m_Allocator(Other.m_Allocator),
          m_Device(Other.m_Device),
          m_Buffer(Other.m_Buffer),
          m_Allocation(Other.m_Allocation),
          m_Size(Other.m_Size) {
        Other.m_Buffer     = nullptr;
        Other.m_Allocation = nullptr;
        Other.m_Size       = 0;
    }

    auto operator=(VulkanHostBuffer&& Other) noexcept -> VulkanHostBuffer& {
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

    VulkanHostBuffer(const VulkanHostBuffer&)     = delete;
    auto operator=(const VulkanHostBuffer&) = delete;

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
            return std::unexpected(ErrorMessage(Format(
                "VulkanHostBuffer upload exceeds size (offset {} + size {} > capacity {})", Offset, Size, m_Size)));
        if (Size == 0) {
            LogWarning("Uploading 0 bytes to a host buffer.");
            return {};
        }

        if (auto Result = vmaCopyMemoryToAllocation(
                m_Allocator, Data, m_Allocation, static_cast<VkDeviceSize>(Offset), static_cast<VkDeviceSize>(Size));
            Result != VK_SUCCESS)
            return std::unexpected(ErrorMessage("vmaCopyMemoryToAllocation failed in VulkanHostBuffer::Upload"));
        return {};
    }

    /// Defer destruction to VulkanTransferCompletionQueue at the given timeline token.
    /// After this call the VulkanHostBuffer is hollowed out (m_Allocation = nullptr)
    /// so its destructor is a no-op. The lambda captures VMA handles by value.
    auto DeferredDelete(VulkanTransferCompletionQueue& Queue, RHIGpuCompletionToken Token) -> void {
        auto Alloc       = m_Allocator;
        auto Buf         = static_cast<VkBuffer>(m_Buffer);
        auto AllocHandle = m_Allocation;
        Queue.EnqueueCallback(Token, [Alloc, Buf, AllocHandle]() {
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
// VulkanDeviceBuffer — device-local buffer
// ═════════════════════════════════════════════════════════════════════════════

/// Internal device-local buffer for GPU-only access.
/// Move-only. Data transferred via CopyFrom with a VulkanHostBuffer staging source.
class VulkanDeviceBuffer {
  public:
    VulkanDeviceBuffer() = default;

    [[nodiscard]] static auto Create(Uint64 Size, vk::BufferUsageFlags Usage, vk::Device Dev, VmaAllocator Alloc)
        -> std::expected<VulkanDeviceBuffer, ErrorMessage> {
        VulkanDeviceBuffer Buf;
        Buf.m_Allocator = Alloc;
        Buf.m_Device    = Dev;
        Buf.m_Size      = Size;

        if (VulkanCapability::Get().GetRayTracingSupport().Available)
            Usage |= vk::BufferUsageFlagBits::eShaderDeviceAddress;

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
            return std::unexpected(ErrorMessage("Failed to create VulkanDeviceBuffer via VMA"));
        }

        return Buf;
    }

    ~VulkanDeviceBuffer() {
        Destroy();
    }

    VulkanDeviceBuffer(VulkanDeviceBuffer&& Other) noexcept
        : m_Allocator(Other.m_Allocator),
          m_Device(Other.m_Device),
          m_Buffer(Other.m_Buffer),
          m_Allocation(Other.m_Allocation),
          m_Size(Other.m_Size) {
        Other.m_Buffer     = nullptr;
        Other.m_Allocation = nullptr;
        Other.m_Size       = 0;
    }

    auto operator=(VulkanDeviceBuffer&& Other) noexcept -> VulkanDeviceBuffer& {
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

    VulkanDeviceBuffer(const VulkanDeviceBuffer&)   = delete;
    auto operator=(const VulkanDeviceBuffer&) = delete;

    [[nodiscard]] auto Get() const -> vk::Buffer {
        return m_Buffer;
    }
    [[nodiscard]] auto GetSize() const -> Uint64 {
        return m_Size;
    }
    [[nodiscard]] auto GetDeviceAddress() const -> vk::DeviceAddress {
        return m_Device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = m_Buffer});
    }

    /// Copy full contents from a VulkanHostBuffer staging source via VulkanImmediateContext.
    /// Copies min(SrcSize, this->Size) bytes and returns the transfer completion token.
    [[nodiscard]] auto CopyFrom(VulkanHostBuffer& Src, VulkanImmediateContext& Ctx)
        -> std::expected<RHIGpuCompletionToken, ErrorMessage> {
        Uint64 CopySize = std::min(Src.GetSize(), m_Size);

        auto CopyResult = Ctx.SubmitTransfer([&](const vk::raii::CommandBuffer& CmdBuf) {
            vk::BufferCopy Region{.srcOffset = 0, .dstOffset = 0, .size = CopySize};
            CmdBuf.copyBuffer(Src.Get(), m_Buffer, {Region});
        });

        if (!CopyResult)
            return std::unexpected(CopyResult.error().Append("VulkanDeviceBuffer::CopyFrom failed"));
        return *CopyResult;
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
// VulkanVertexBuffer — typed vertex buffer
// ═════════════════════════════════════════════════════════════════════════════

class VulkanVertexBuffer final : public RHIVertexBuffer {
  public:
    VulkanVertexBuffer(SPtr<VulkanDeviceBuffer> Buf, VulkanDeletionQueue& Queue, Uint32 Stride, Uint64 VertexCount)
        : m_Buffer(std::move(Buf)), m_DeletionQueue(&Queue), m_Stride(Stride), m_VertexCount(VertexCount) {}

    ~VulkanVertexBuffer() override {
        if (m_DeletionQueue) {
            m_DeletionQueue->Enqueue(GetLastUsageToken(), [Buf = m_Buffer]() {});
        }
    }

    /// Static factory: creates staging buffer, uploads data, copies to
    /// device-local buffer via VulkanImmediateContext, and defers staging destruction
    /// to VulkanTransferCompletionQueue.
    [[nodiscard]] static auto Create(const RHIVertexBufferDesc& Desc,
                                     VmaAllocator                 Alloc,
                                     vk::Device                   Dev,
                                     VulkanImmediateContext&            ImmCtx,
                                     VulkanTransferCompletionQueue&     CompletionQueue,
                                     VulkanDeletionQueue&               DelQueue)
        -> std::expected<RHIVertexBufferCreateResult, ErrorMessage> {
        if (!Desc.Data)
            return std::unexpected(ErrorMessage("VulkanVertexBuffer::Create: data pointer is null"));
        if (Desc.VertexCount == 0)
            return std::unexpected(ErrorMessage("VulkanVertexBuffer::Create: vertex count is zero"));
        if (Desc.VertexCount > std::numeric_limits<Uint32>::max())
            return std::unexpected(ErrorMessage("VulkanVertexBuffer::Create: vertex count exceeds Vulkan draw limit"));
        if (Desc.Stride == 0)
            return std::unexpected(ErrorMessage("VulkanVertexBuffer::Create: vertex stride is zero"));

        Uint64 Size  = Desc.VertexCount * Desc.Stride;
        Uint64 VCnt  = Desc.VertexCount;
        Uint32 Strid = Desc.Stride;

        // ── Staging buffer ──────────────────────────────────────────────
        VulkanHostBuffer Staging;
        auto       StagingRes = VulkanHostBuffer::Create(Size, vk::BufferUsageFlagBits::eTransferSrc, Dev, Alloc);
        if (!StagingRes)
            return std::unexpected(StagingRes.error().Append("VulkanVertexBuffer::Create: staging creation failed"));
        Staging = std::move(*StagingRes);

        if (auto R = Staging.Upload(Desc.Data, Size); !R)
            return std::unexpected(R.error().Append("VulkanVertexBuffer::Create: staging upload failed"));

        // ── Device buffer ────────────────────────────────────────────────
        VulkanDeviceBuffer DevBuf;
        auto Usage = vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
                     vk::BufferUsageFlagBits::eTransferDst;
        if (VulkanCapability::Get().GetRayTracingSupport().Available)
            Usage |= vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;
        auto DevRes = VulkanDeviceBuffer::Create(Size, Usage, Dev, Alloc);
        if (!DevRes)
            return std::unexpected(DevRes.error().Append("VulkanVertexBuffer::Create: device buffer creation failed"));
        DevBuf = std::move(*DevRes);

        // ── Copy staging -> device with timeline signal ────────────────
        auto CopyToken = DevBuf.CopyFrom(Staging, ImmCtx);
        if (!CopyToken)
            return std::unexpected(CopyToken.error().Append("VulkanVertexBuffer::Create: staging copy failed"));

        // ── Defer staging destruction ──────────────────────────────────
        Staging.DeferredDelete(CompletionQueue, *CopyToken);

        // ── Move VulkanDeviceBuffer into delayed-deletion ownership ──────────
        // DevBuf is a local; move it onto the heap so VulkanVertexBuffer's
        // destructor can hand it to VulkanDeletionQueue. The moved-from local's
        // destructor is a no-op.
        return RHIVertexBufferCreateResult{
            .Buffer = std::make_unique<VulkanVertexBuffer>(
                std::make_shared<VulkanDeviceBuffer>(std::move(DevBuf)), DelQueue, Strid, VCnt),
            .UploadCompletion = *CopyToken,
        };
    }

    [[nodiscard]] auto GetVkBuffer() const -> vk::Buffer {
        return m_Buffer->Get();
    }
    [[nodiscard]] auto GetStride() const -> Uint32 {
        return m_Stride;
    }
    [[nodiscard]] auto GetVertexCount() const -> Uint64 {
        return m_VertexCount;
    }

    VulkanVertexBuffer(const VulkanVertexBuffer&)                    = delete;
    auto operator=(const VulkanVertexBuffer&) -> VulkanVertexBuffer& = delete;
    VulkanVertexBuffer(VulkanVertexBuffer&&)                         = delete;
    auto operator=(VulkanVertexBuffer&&) -> VulkanVertexBuffer&      = delete;

  private:
    SPtr<VulkanDeviceBuffer> m_Buffer        = nullptr;
    VulkanDeletionQueue*     m_DeletionQueue = nullptr;
    Uint32             m_Stride        = 0;
    Uint64             m_VertexCount   = 0;
};

// ═════════════════════════════════════════════════════════════════════════════
// VulkanIndexBuffer — typed index buffer
// ═════════════════════════════════════════════════════════════════════════════

class VulkanIndexBuffer final : public RHIIndexBuffer {
  public:
    VulkanIndexBuffer(SPtr<VulkanDeviceBuffer> Buf, VulkanDeletionQueue& Queue, Uint64 IndexCount)
        : m_Buffer(std::move(Buf)), m_DeletionQueue(&Queue), m_IndexCount(IndexCount) {}

    ~VulkanIndexBuffer() override {
        if (m_DeletionQueue) {
            m_DeletionQueue->Enqueue(GetLastUsageToken(), [Buf = m_Buffer]() {});
        }
    }

    /// Static factory: same pattern as VulkanVertexBuffer::Create.
    /// Index type is hardcoded to uint32 (eUint32).  uint16 is not supported.
    [[nodiscard]] static auto Create(const RHIIndexBufferDesc& Desc,
                                     VmaAllocator                Alloc,
                                     vk::Device                  Dev,
                                     VulkanImmediateContext&           ImmCtx,
                                     VulkanTransferCompletionQueue&    CompletionQueue,
                                     VulkanDeletionQueue&              DelQueue)
        -> std::expected<RHIIndexBufferCreateResult, ErrorMessage> {
        if (!Desc.Data)
            return std::unexpected(ErrorMessage("VulkanIndexBuffer::Create: data pointer is null"));
        if (Desc.IndexCount == 0)
            return std::unexpected(ErrorMessage("VulkanIndexBuffer::Create: index count is zero"));
        if (Desc.IndexCount > std::numeric_limits<Uint32>::max())
            return std::unexpected(ErrorMessage("VulkanIndexBuffer::Create: index count exceeds Vulkan draw limit"));

        Uint64 IndexCount = Desc.IndexCount;
        Uint64 Size       = IndexCount * 4ULL;

        VulkanHostBuffer Staging;
        auto       StagingRes = VulkanHostBuffer::Create(Size, vk::BufferUsageFlagBits::eTransferSrc, Dev, Alloc);
        if (!StagingRes)
            return std::unexpected(StagingRes.error().Append("VulkanIndexBuffer::Create: staging creation failed"));
        Staging = std::move(*StagingRes);

        if (auto R = Staging.Upload(Desc.Data, Size); !R)
            return std::unexpected(R.error().Append("VulkanIndexBuffer::Create: staging upload failed"));

        VulkanDeviceBuffer DevBuf;
        auto Usage = vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
                     vk::BufferUsageFlagBits::eTransferDst;
        if (VulkanCapability::Get().GetRayTracingSupport().Available)
            Usage |= vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;
        auto DevRes = VulkanDeviceBuffer::Create(Size, Usage, Dev, Alloc);
        if (!DevRes)
            return std::unexpected(DevRes.error().Append("VulkanIndexBuffer::Create: device buffer creation failed"));
        DevBuf = std::move(*DevRes);

        auto CopyToken = DevBuf.CopyFrom(Staging, ImmCtx);
        if (!CopyToken)
            return std::unexpected(CopyToken.error().Append("VulkanIndexBuffer::Create: staging copy failed"));

        Staging.DeferredDelete(CompletionQueue, *CopyToken);

        // ── Move VulkanDeviceBuffer into delayed-deletion ownership ──────────
        return RHIIndexBufferCreateResult{
            .Buffer =
                std::make_unique<VulkanIndexBuffer>(std::make_shared<VulkanDeviceBuffer>(std::move(DevBuf)), DelQueue, IndexCount),
            .UploadCompletion = *CopyToken,
        };
    }

    [[nodiscard]] auto GetVkBuffer() const -> vk::Buffer {
        return m_Buffer->Get();
    }
    [[nodiscard]] auto GetIndexCount() const -> Uint64 {
        return m_IndexCount;
    }

    VulkanIndexBuffer(const VulkanIndexBuffer&)                    = delete;
    auto operator=(const VulkanIndexBuffer&) -> VulkanIndexBuffer& = delete;
    VulkanIndexBuffer(VulkanIndexBuffer&&)                         = delete;
    auto operator=(VulkanIndexBuffer&&) -> VulkanIndexBuffer&      = delete;

  private:
    SPtr<VulkanDeviceBuffer> m_Buffer        = nullptr;
    VulkanDeletionQueue*     m_DeletionQueue = nullptr;
    Uint64             m_IndexCount    = 0;
};

// ═════════════════════════════════════════════════════════════════════════════
// VulkanConstantBuffer — logical RHI constant block with arena offset
// ═════════════════════════════════════════════════════════════════════════════

/// Vulkan backend constant-buffer handle.
/// Owns no VkBuffer; it records the per-frame offsets reserved in the shared
/// VulkanUniformBufferArena.
class VulkanConstantBuffer final : public RHIConstantBuffer {
  public:
    VulkanConstantBuffer(const RHIConstantBufferDesc& Desc, std::vector<Uint32> ArenaOffsets)
        : RHIConstantBuffer(Desc) {
        m_ArenaOffsets = std::move(ArenaOffsets);
    }

    [[nodiscard]] auto GetArenaOffset(Uint32 FrameIndex) const -> Uint32 {
        return m_ArenaOffsets[FrameIndex];
    }

  private:
    std::vector<Uint32> m_ArenaOffsets = {};
};

// ═════════════════════════════════════════════════════════════════════════════
// VulkanUniformBufferArena — shared dynamic uniform storage
// ═════════════════════════════════════════════════════════════════════════════

/// Shared uniform-buffer arena used by dynamic UBO descriptors.
class VulkanUniformBufferArena final {
  public:
    VulkanUniformBufferArena() = default;

    explicit VulkanUniformBufferArena(VulkanHostBuffer&& Buffer) {
        m_Buffer = std::move(Buffer);
        m_Size   = m_Buffer.GetSize();
    }

    [[nodiscard]] static auto Create(Uint64 Size, vk::Device Dev, VmaAllocator Alloc)
        -> std::expected<VulkanUniformBufferArena, ErrorMessage> {
        if (Size == 0)
            return std::unexpected(ErrorMessage("VulkanUniformBufferArena::Create: size must be greater than zero"));

        auto Buffer = VulkanHostBuffer::Create(Size, vk::BufferUsageFlagBits::eUniformBuffer, Dev, Alloc);
        if (!Buffer)
            return std::unexpected(Buffer.error().Append("VulkanUniformBufferArena::Create: VulkanHostBuffer creation failed"));
        return VulkanUniformBufferArena(std::move(*Buffer));
    }

    VulkanUniformBufferArena(const VulkanUniformBufferArena&)                       = delete;
    auto operator=(const VulkanUniformBufferArena&) -> VulkanUniformBufferArena&    = delete;
    VulkanUniformBufferArena(VulkanUniformBufferArena&&) noexcept                   = default;
    auto operator=(VulkanUniformBufferArena&&) noexcept -> VulkanUniformBufferArena& = default;

    [[nodiscard]] auto Allocate(Uint64 Size) -> std::expected<Uint32, ErrorMessage> {
        const auto MaxRange =
            static_cast<Uint64>(VulkanCapability::Get().GetProperties().limits.maxUniformBufferRange);
        if (Size > MaxRange) {
            return std::unexpected(
                ErrorMessage(Format("VulkanUniformBufferArena allocation exceeds maxUniformBufferRange ({} bytes > {})",
                                          Size,
                                          MaxRange)));
        }

        const auto Alignment =
            static_cast<Uint64>(VulkanCapability::Get().GetProperties().limits.minUniformBufferOffsetAlignment);

        auto LogicalOffset = AlignUp(m_NextLogicalOffset, Alignment);
        if (!LogicalOffset)
            return std::unexpected(
                LogicalOffset.error().Append("VulkanUniformBufferArena::Allocate: offset alignment failed"));
        if (*LogicalOffset > std::numeric_limits<Uint64>::max() - Size)
            return std::unexpected(ErrorMessage("VulkanUniformBufferArena allocation size overflow"));
        if (*LogicalOffset + Size > m_Size) {
            return std::unexpected(ErrorMessage(
                Format("VulkanUniformBufferArena allocation exceeds capacity (offset {} + size {} > {})",
                             *LogicalOffset,
                             Size,
                             m_Size)));
        }
        if (*LogicalOffset > std::numeric_limits<Uint32>::max())
            return std::unexpected(ErrorMessage("VulkanUniformBufferArena offset exceeds dynamic offset range"));

        m_NextLogicalOffset = *LogicalOffset + Size;
        return static_cast<Uint32>(*LogicalOffset);
    }

    [[nodiscard]] auto Write(const void* Data, Uint64 Size, Uint32 Offset) -> std::expected<void, ErrorMessage> {
        if (static_cast<Uint64>(Offset) + Size > m_Size) {
            return std::unexpected(ErrorMessage(
                Format("VulkanUniformBufferArena: write exceeds capacity (offset {} + size {} > capacity {})",
                             Offset,
                             Size,
                             m_Size)));
        }
        if (auto R = m_Buffer.Upload(Data, Size, Offset); !R)
            return std::unexpected(R.error().Append("VulkanUniformBufferArena::Write failed"));
        return {};
    }

    [[nodiscard]] auto GetVkBuffer() const -> vk::Buffer {
        return m_Buffer.Get();
    }

    [[nodiscard]] auto GetSize() const -> Uint64 {
        return m_Size;
    }

  private:
    [[nodiscard]] static auto AlignUp(Uint64 Value, Uint64 Alignment) -> std::expected<Uint64, ErrorMessage> {
        if (Alignment == 0)
            return Value;
        if (Value > std::numeric_limits<Uint64>::max() - (Alignment - 1))
            return std::unexpected(ErrorMessage("VulkanUniformBufferArena alignment overflow"));
        return ((Value + Alignment - 1) / Alignment) * Alignment;
    }

    VulkanHostBuffer m_Buffer            = {};
    Uint64     m_Size              = 0;
    Uint64     m_NextLogicalOffset = 0;
};


/// Shared per-frame uniform-buffer arena for values that must remain distinct
/// between individual draw commands.
class VulkanTransientUniformBufferArena final {
  public:
    VulkanTransientUniformBufferArena() = default;

    explicit VulkanTransientUniformBufferArena(VulkanHostBuffer&& Buffer, Uint64 FrameCapacity, Uint32 FramesInFlight)
        : m_Buffer(std::move(Buffer)), m_FrameCapacity(FrameCapacity), m_NextOffsets(FramesInFlight) {}

    [[nodiscard]] static auto Create(Uint64 CapacityPerFrame, vk::Device Dev, VmaAllocator Alloc, Uint32 FramesInFlight)
        -> std::expected<VulkanTransientUniformBufferArena, ErrorMessage> {
        if (CapacityPerFrame == 0)
            return std::unexpected(ErrorMessage("VulkanTransientUniformBufferArena::Create: capacity must be greater than zero"));
        if (FramesInFlight == 0)
            return std::unexpected(ErrorMessage("VulkanTransientUniformBufferArena::Create: frames-in-flight must be greater than zero"));
        if (CapacityPerFrame > std::numeric_limits<Uint64>::max() / FramesInFlight)
            return std::unexpected(ErrorMessage("VulkanTransientUniformBufferArena::Create: total buffer size overflow"));

        auto Buffer = VulkanHostBuffer::Create(CapacityPerFrame * FramesInFlight, vk::BufferUsageFlagBits::eUniformBuffer, Dev, Alloc);
        if (!Buffer)
            return std::unexpected(Buffer.error().Append("VulkanTransientUniformBufferArena::Create: VulkanHostBuffer creation failed"));
        return VulkanTransientUniformBufferArena(std::move(*Buffer), CapacityPerFrame, FramesInFlight);
    }

    VulkanTransientUniformBufferArena(const VulkanTransientUniformBufferArena&)                       = delete;
    auto operator=(const VulkanTransientUniformBufferArena&) -> VulkanTransientUniformBufferArena&    = delete;
    VulkanTransientUniformBufferArena(VulkanTransientUniformBufferArena&&) noexcept                   = default;
    auto operator=(VulkanTransientUniformBufferArena&&) noexcept -> VulkanTransientUniformBufferArena& = default;

    [[nodiscard]] auto BeginFrame(Uint32 FrameIndex) -> std::expected<void, ErrorMessage> {
        if (FrameIndex >= m_NextOffsets.size())
            return std::unexpected(ErrorMessage("VulkanTransientUniformBufferArena::BeginFrame: frame index is out of range"));
        m_NextOffsets[FrameIndex] = static_cast<Uint64>(FrameIndex) * m_FrameCapacity;
        return {};
    }

    [[nodiscard]] auto Allocate(Uint32 FrameIndex, Uint64 Size) -> std::expected<Uint32, ErrorMessage> {
        if (FrameIndex >= m_NextOffsets.size())
            return std::unexpected(ErrorMessage("VulkanTransientUniformBufferArena::Allocate: frame index is out of range"));
        if (Size == 0)
            return std::unexpected(ErrorMessage("VulkanTransientUniformBufferArena::Allocate: size must be greater than zero"));

        const auto MaxRange =
            static_cast<Uint64>(VulkanCapability::Get().GetProperties().limits.maxUniformBufferRange);
        if (Size > MaxRange) {
            return std::unexpected(ErrorMessage(Format(
                "VulkanTransientUniformBufferArena allocation exceeds maxUniformBufferRange ({} bytes > {})", Size, MaxRange)));
        }

        const auto Alignment =
            static_cast<Uint64>(VulkanCapability::Get().GetProperties().limits.minUniformBufferOffsetAlignment);
        auto Offset = AlignUp(m_NextOffsets[FrameIndex], Alignment);
        if (!Offset)
            return std::unexpected(Offset.error().Append("VulkanTransientUniformBufferArena::Allocate: offset alignment failed"));
        if (*Offset > std::numeric_limits<Uint64>::max() - Size)
            return std::unexpected(ErrorMessage("VulkanTransientUniformBufferArena allocation size overflow"));

        const auto FrameEnd = (static_cast<Uint64>(FrameIndex) + 1) * m_FrameCapacity;
        if (*Offset + Size > FrameEnd) {
            return std::unexpected(ErrorMessage(Format(
                "VulkanTransientUniformBufferArena allocation exceeds frame capacity (offset {} + size {} > frame end {})",
                *Offset,
                Size,
                FrameEnd)));
        }
        if (*Offset > std::numeric_limits<Uint32>::max())
            return std::unexpected(ErrorMessage("VulkanTransientUniformBufferArena offset exceeds dynamic offset range"));

        m_NextOffsets[FrameIndex] = *Offset + Size;
        return static_cast<Uint32>(*Offset);
    }

    [[nodiscard]] auto Write(const void* Data, Uint64 Size, Uint32 Offset) -> std::expected<void, ErrorMessage> {
        if (auto R = m_Buffer.Upload(Data, Size, Offset); !R)
            return std::unexpected(R.error().Append("VulkanTransientUniformBufferArena::Write failed"));
        return {};
    }

    [[nodiscard]] auto GetVkBuffer() const -> vk::Buffer {
        return m_Buffer.Get();
    }

  private:
    [[nodiscard]] static auto AlignUp(Uint64 Value, Uint64 Alignment) -> std::expected<Uint64, ErrorMessage> {
        if (Alignment == 0)
            return Value;
        if (Value > std::numeric_limits<Uint64>::max() - (Alignment - 1))
            return std::unexpected(ErrorMessage("VulkanTransientUniformBufferArena alignment overflow"));
        return ((Value + Alignment - 1) / Alignment) * Alignment;
    }

    VulkanHostBuffer          m_Buffer        = {};
    Uint64              m_FrameCapacity = 0;
    std::vector<Uint64> m_NextOffsets   = {};
};

} // namespace SoulEngine
