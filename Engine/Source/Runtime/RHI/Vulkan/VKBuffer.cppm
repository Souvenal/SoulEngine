module;

#include <vk_mem_alloc.h>

export module Vulkan:Buffer;

import Core;
import vulkan;
import RHI;
import std;

import :Capability;
import :Types;
import :Context;

namespace SoulEngine {

// ═════════════════════════════════════════════════════════════════════════════
// VulkanHostBuffer — mappable staging buffer
// ═════════════════════════════════════════════════════════════════════════════

/// Internal mappable buffer for staging uploads.
/// Created via static Create(). Move-only.
class VulkanHostBuffer {
  public:
    VulkanHostBuffer() = default;

    [[nodiscard]] static auto Create(Uint64 Size, vk::BufferUsageFlags Usage, vk::raii::Device& Dev, VmaAllocator Alloc)
        -> std::expected<VulkanHostBuffer, ErrorMessage> {
        VulkanHostBuffer Buf;
        Buf.m_Allocator = Alloc;
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

        if (static_cast<bool>(Usage & vk::BufferUsageFlagBits::eShaderDeviceAddress) &&
            VulkanCapability::Get().GetFeatures<vk::PhysicalDeviceVulkan12Features>().bufferDeviceAddress) {
            const auto AddressInfo = vk::BufferDeviceAddressInfo{.buffer = Buf.m_Buffer};
            Buf.m_DeviceAddress    = Dev.getBufferAddress(AddressInfo);
        }

        return Buf;
    }

    ~VulkanHostBuffer() {
        Destroy();
    }

    VulkanHostBuffer(VulkanHostBuffer&& Other) noexcept
        : m_Allocator(Other.m_Allocator),
          m_Buffer(Other.m_Buffer),
          m_Allocation(Other.m_Allocation),
          m_DeviceAddress(Other.m_DeviceAddress),
          m_Size(Other.m_Size) {
        Other.m_Buffer        = nullptr;
        Other.m_Allocation    = nullptr;
        Other.m_DeviceAddress = 0;
        Other.m_Size          = 0;
    }

    auto operator=(VulkanHostBuffer&& Other) noexcept -> VulkanHostBuffer& {
        if (this != &Other) {
            std::swap(m_Allocator, Other.m_Allocator);
            std::swap(m_Buffer, Other.m_Buffer);
            std::swap(m_Allocation, Other.m_Allocation);
            std::swap(m_DeviceAddress, Other.m_DeviceAddress);
            std::swap(m_Size, Other.m_Size);
        }
        return *this;
    }

    VulkanHostBuffer(const VulkanHostBuffer&) = delete;
    auto operator=(const VulkanHostBuffer&)   = delete;

    [[nodiscard]] auto Get() const -> vk::Buffer {
        return m_Buffer;
    }
    [[nodiscard]] auto GetSize() const -> Uint64 {
        return m_Size;
    }
    [[nodiscard]] auto GetDeviceAddress() const -> vk::DeviceAddress {
        return m_DeviceAddress;
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

  private:
    auto Destroy() -> void {
        if (m_Allocation) {
            vmaDestroyBuffer(m_Allocator, static_cast<VkBuffer>(m_Buffer), m_Allocation);
        }
    }

    VmaAllocator      m_Allocator     = nullptr;
    vk::Buffer        m_Buffer        = nullptr;
    VmaAllocation     m_Allocation    = nullptr;
    vk::DeviceAddress m_DeviceAddress = 0;
    Uint64            m_Size          = 0;
};

// ═════════════════════════════════════════════════════════════════════════════
// VulkanDeviceBuffer — device-local buffer
// ═════════════════════════════════════════════════════════════════════════════

/// Internal device-local buffer for GPU-only access.
/// Move-only. Data transferred via CopyFrom with a VulkanHostBuffer staging source.
class VulkanDeviceBuffer {
  public:
    VulkanDeviceBuffer() = default;

    [[nodiscard]] static auto Create(Uint64 Size, vk::BufferUsageFlags Usage, const VulkanResourceContext& Context)
        -> std::expected<VulkanDeviceBuffer, ErrorMessage> {
        VulkanDeviceBuffer Buf;
        Buf.m_Allocator = Context.Allocator;
        const std::array QueueFamilies{Context.GraphicsFamily, Context.TransferFamily};
        const std::span  SharingFamilies = Context.GraphicsFamily != Context.TransferFamily
                                               ? std::span<const Uint32>{QueueFamilies}
                                               : std::span<const Uint32>{};
        Buf.m_Size                       = Size;

        const bool bBufferDeviceAddress =
            VulkanCapability::Get().GetFeatures<vk::PhysicalDeviceVulkan12Features>().bufferDeviceAddress;
        if (bBufferDeviceAddress)
            Usage |= vk::BufferUsageFlagBits::eShaderDeviceAddress;

        vk::BufferCreateInfo BufCI{
            .size        = Size,
            .usage       = Usage,
            .sharingMode = SharingFamilies.size() > 1 ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive,
            .queueFamilyIndexCount = static_cast<Uint32>(SharingFamilies.size()),
            .pQueueFamilyIndices   = SharingFamilies.data(),
        };

        VmaAllocationCreateInfo AllocInfo{
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        };

        if (vmaCreateBuffer(Context.Allocator,
                            reinterpret_cast<VkBufferCreateInfo*>(&BufCI),
                            &AllocInfo,
                            reinterpret_cast<VkBuffer*>(&Buf.m_Buffer),
                            &Buf.m_Allocation,
                            nullptr) != VK_SUCCESS) {
            return std::unexpected(ErrorMessage("Failed to create VulkanDeviceBuffer via VMA"));
        }

        if (bBufferDeviceAddress) {
            const auto AddressInfo = vk::BufferDeviceAddressInfo{.buffer = Buf.m_Buffer};
            Buf.m_DeviceAddress    = Context.Device.getBufferAddress(AddressInfo);
        }

        return Buf;
    }

    ~VulkanDeviceBuffer() {
        Destroy();
    }

    VulkanDeviceBuffer(VulkanDeviceBuffer&& Other) noexcept
        : m_Allocator(Other.m_Allocator),
          m_Buffer(Other.m_Buffer),
          m_Allocation(Other.m_Allocation),
          m_DeviceAddress(Other.m_DeviceAddress),
          m_Size(Other.m_Size) {
        Other.m_Buffer        = nullptr;
        Other.m_Allocation    = nullptr;
        Other.m_DeviceAddress = 0;
        Other.m_Size          = 0;
    }

    auto operator=(VulkanDeviceBuffer&& Other) noexcept -> VulkanDeviceBuffer& {
        if (this != &Other) {
            std::swap(m_Allocator, Other.m_Allocator);
            std::swap(m_Buffer, Other.m_Buffer);
            std::swap(m_Allocation, Other.m_Allocation);
            std::swap(m_DeviceAddress, Other.m_DeviceAddress);
            std::swap(m_Size, Other.m_Size);
        }
        return *this;
    }

    VulkanDeviceBuffer(const VulkanDeviceBuffer&) = delete;
    auto operator=(const VulkanDeviceBuffer&)     = delete;

    [[nodiscard]] auto Get() const -> vk::Buffer {
        return m_Buffer;
    }
    [[nodiscard]] auto GetSize() const -> Uint64 {
        return m_Size;
    }
    [[nodiscard]] auto GetDeviceAddress() const -> vk::DeviceAddress {
        return m_DeviceAddress;
    }

    /// Copy full contents from a VulkanHostBuffer staging source via VulkanImmediateContext.
    /// Copies min(SrcSize, this->Size) bytes and returns the transfer completion token.
    [[nodiscard]] auto CopyFrom(VulkanHostBuffer& Src,
                                VulkanImmediateContext& Ctx,
                                VulkanImmediateContext::CompletionDesc Completion)
        -> std::expected<void, ErrorMessage> {
        const Uint64 CopySize = std::min(Src.GetSize(), m_Size);
        if (auto R = Ctx.Submit(VulkanImmediateQueue::Transfer,
                                vk::PipelineStageFlagBits2::eTransfer,
                                [&](const vk::raii::CommandBuffer& CmdBuf) {
                                    vk::BufferCopy Region{.srcOffset = 0, .dstOffset = 0, .size = CopySize};
                                    CmdBuf.copyBuffer(Src.Get(), m_Buffer, {Region});
                                },
                                std::move(Completion));
            !R) {
            return std::unexpected(R.error().Append("VulkanDeviceBuffer::CopyFrom failed"));
        }
        return {};
    }

  private:
    auto Destroy() -> void {
        if (m_Allocation)
            vmaDestroyBuffer(m_Allocator, static_cast<VkBuffer>(m_Buffer), m_Allocation);
    }

    VmaAllocator      m_Allocator     = nullptr;
    vk::Buffer        m_Buffer        = nullptr;
    VmaAllocation     m_Allocation    = nullptr;
    vk::DeviceAddress m_DeviceAddress = 0;
    Uint64            m_Size          = 0;
};

// ═════════════════════════════════════════════════════════════════════════════
// VulkanVertexBuffer — typed vertex buffer
// ═════════════════════════════════════════════════════════════════════════════

class VulkanVertexBuffer final : public RHIVertexBuffer {
  public:
    VulkanVertexBuffer(SPtr<VulkanDeviceBuffer> Buf, const RHIVertexBufferDesc& Desc)
        : RHIVertexBuffer(Desc), m_Buffer(std::move(Buf)) {}

    ~VulkanVertexBuffer() override = default;

    /// Static factory: creates staging buffer, uploads data, copies to
    /// device-local buffer via VulkanImmediateContext, and defers staging destruction
    /// to VulkanTransferCompletionQueue.
    [[nodiscard]] static auto Create(const VulkanResourceContext& Context,
                                     const RHIVertexBufferDesc& Desc,
                                     VulkanImmediateContext::CompletionFn OnReady)
        -> std::expected<UPtr<VulkanVertexBuffer>, ErrorMessage> {
        if (Desc.Data.empty())
            return std::unexpected(ErrorMessage("VulkanVertexBuffer::Create: data is empty"));
        if (Desc.VertexCount == 0)
            return std::unexpected(ErrorMessage("VulkanVertexBuffer::Create: vertex count is zero"));
        if (Desc.VertexCount > std::numeric_limits<Uint32>::max())
            return std::unexpected(ErrorMessage("VulkanVertexBuffer::Create: vertex count exceeds Vulkan draw limit"));
        if (Desc.Stride == 0)
            return std::unexpected(ErrorMessage("VulkanVertexBuffer::Create: vertex stride is zero"));
        if (Desc.VertexCount > std::numeric_limits<Uint64>::max() / Desc.Stride)
            return std::unexpected(ErrorMessage("VulkanVertexBuffer::Create: source data size overflows Uint64"));

        const Uint64 Size = Desc.VertexCount * Desc.Stride;
        if (Desc.Data.size_bytes() != Size)
            return std::unexpected(ErrorMessage("VulkanVertexBuffer::Create: data size does not match vertex layout"));

        auto StagingRes = VulkanHostBuffer::Create(Size, vk::BufferUsageFlagBits::eTransferSrc, Context.Device, Context.Allocator);
        if (!StagingRes)
            return std::unexpected(StagingRes.error().Append("VulkanVertexBuffer::Create: staging creation failed"));
        auto Staging = std::make_shared<VulkanHostBuffer>(std::move(*StagingRes));
        if (auto R = Staging->Upload(Desc.Data.data(), Desc.Data.size_bytes()); !R)
            return std::unexpected(R.error().Append("VulkanVertexBuffer::Create: staging upload failed"));

        auto Usage = vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
                     vk::BufferUsageFlagBits::eTransferDst;
        if (VulkanCapability::Get().IsRayTracingAvailable())
            Usage |= vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;
        auto DevRes = VulkanDeviceBuffer::Create(Size, Usage, Context);
        if (!DevRes)
            return std::unexpected(DevRes.error().Append("VulkanVertexBuffer::Create: device buffer creation failed"));
        auto Buffer = std::make_shared<VulkanDeviceBuffer>(std::move(*DevRes));

        auto Completion = VulkanImmediateContext::CompletionDesc{
            .ConsumerQueue = VulkanImmediateQueue::Graphics,
            .OnComplete = [Staging, Buffer, OnReady = std::move(OnReady)]() mutable {
                if (OnReady)
                    OnReady();
            },
        };
        if (auto R = Buffer->CopyFrom(*Staging, Context.Immediate, std::move(Completion)); !R)
            return std::unexpected(R.error().Append("VulkanVertexBuffer::Create: staging copy failed"));

        return std::make_unique<VulkanVertexBuffer>(std::move(Buffer), Desc);
    }
    [[nodiscard]] auto GetVkBuffer() const -> vk::Buffer {
        return m_Buffer->Get();
    }
    [[nodiscard]] auto GetDeviceAddress() const -> vk::DeviceAddress {
        return m_Buffer->GetDeviceAddress();
    }
    VulkanVertexBuffer(const VulkanVertexBuffer&)                    = delete;
    auto operator=(const VulkanVertexBuffer&) -> VulkanVertexBuffer& = delete;
    VulkanVertexBuffer(VulkanVertexBuffer&&)                         = delete;
    auto operator=(VulkanVertexBuffer&&) -> VulkanVertexBuffer&      = delete;

  private:
    SPtr<VulkanDeviceBuffer> m_Buffer = nullptr;
};

// ═════════════════════════════════════════════════════════════════════════════
// VulkanIndexBuffer — typed index buffer
// ═════════════════════════════════════════════════════════════════════════════

class VulkanIndexBuffer final : public RHIIndexBuffer {
  public:
    VulkanIndexBuffer(SPtr<VulkanDeviceBuffer> Buf, const RHIIndexBufferDesc& Desc)
        : RHIIndexBuffer(Desc), m_Buffer(std::move(Buf)) {}

    ~VulkanIndexBuffer() override = default;

    /// Static factory: same pattern as VulkanVertexBuffer::Create.
    /// Index type is hardcoded to uint32 (eUint32).  uint16 is not supported.
    [[nodiscard]] static auto Create(const VulkanResourceContext& Context,
                                     const RHIIndexBufferDesc& Desc,
                                     VulkanImmediateContext::CompletionFn OnReady)
        -> std::expected<UPtr<VulkanIndexBuffer>, ErrorMessage> {
        if (Desc.Data.empty())
            return std::unexpected(ErrorMessage("VulkanIndexBuffer::Create: data is empty"));
        if (Desc.IndexCount == 0)
            return std::unexpected(ErrorMessage("VulkanIndexBuffer::Create: index count is zero"));
        if (Desc.IndexCount > std::numeric_limits<Uint32>::max())
            return std::unexpected(ErrorMessage("VulkanIndexBuffer::Create: index count exceeds Vulkan draw limit"));

        const Uint64 Size = Desc.IndexCount * 4ULL;
        if (Desc.Data.size_bytes() != Size)
            return std::unexpected(ErrorMessage("VulkanIndexBuffer::Create: data size does not match index count"));

        auto StagingRes = VulkanHostBuffer::Create(Size, vk::BufferUsageFlagBits::eTransferSrc, Context.Device, Context.Allocator);
        if (!StagingRes)
            return std::unexpected(StagingRes.error().Append("VulkanIndexBuffer::Create: staging creation failed"));
        auto Staging = std::make_shared<VulkanHostBuffer>(std::move(*StagingRes));
        if (auto R = Staging->Upload(Desc.Data.data(), Desc.Data.size_bytes()); !R)
            return std::unexpected(R.error().Append("VulkanIndexBuffer::Create: staging upload failed"));

        auto Usage = vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
                     vk::BufferUsageFlagBits::eTransferDst;
        if (VulkanCapability::Get().IsRayTracingAvailable())
            Usage |= vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;
        auto DevRes = VulkanDeviceBuffer::Create(Size, Usage, Context);
        if (!DevRes)
            return std::unexpected(DevRes.error().Append("VulkanIndexBuffer::Create: device buffer creation failed"));
        auto Buffer = std::make_shared<VulkanDeviceBuffer>(std::move(*DevRes));

        auto Completion = VulkanImmediateContext::CompletionDesc{
            .ConsumerQueue = VulkanImmediateQueue::Graphics,
            .OnComplete = [Staging, Buffer, OnReady = std::move(OnReady)]() mutable {
                if (OnReady)
                    OnReady();
            },
        };
        if (auto R = Buffer->CopyFrom(*Staging, Context.Immediate, std::move(Completion)); !R)
            return std::unexpected(R.error().Append("VulkanIndexBuffer::Create: staging copy failed"));

        return std::make_unique<VulkanIndexBuffer>(std::move(Buffer), Desc);
    }
    [[nodiscard]] auto GetVkBuffer() const -> vk::Buffer {
        return m_Buffer->Get();
    }
    [[nodiscard]] auto GetDeviceAddress() const -> vk::DeviceAddress {
        return m_Buffer->GetDeviceAddress();
    }
    VulkanIndexBuffer(const VulkanIndexBuffer&)                    = delete;
    auto operator=(const VulkanIndexBuffer&) -> VulkanIndexBuffer& = delete;
    VulkanIndexBuffer(VulkanIndexBuffer&&)                         = delete;
    auto operator=(VulkanIndexBuffer&&) -> VulkanIndexBuffer&      = delete;

  private:
    SPtr<VulkanDeviceBuffer> m_Buffer = nullptr;
};

/// Shared per-frame uniform-buffer arena for values that must remain distinct
/// between individual draw commands.
class VulkanTransientUniformArena final {
  public:
    VulkanTransientUniformArena() = default;

    explicit VulkanTransientUniformArena(VulkanHostBuffer&& Buffer, Uint64 FrameCapacity, Uint32 FramesInFlight)
        : m_Buffer(std::move(Buffer)), m_FrameCapacity(FrameCapacity), m_NextOffsets(FramesInFlight) {}

    [[nodiscard]] static auto
    Create(Uint64 CapacityPerFrame, vk::raii::Device& Dev, VmaAllocator Alloc, Uint32 FramesInFlight)
        -> std::expected<VulkanTransientUniformArena, ErrorMessage> {
        if (CapacityPerFrame == 0)
            return std::unexpected(
                ErrorMessage("VulkanTransientUniformArena::Create: capacity must be greater than zero"));
        if (FramesInFlight == 0)
            return std::unexpected(
                ErrorMessage("VulkanTransientUniformArena::Create: frames-in-flight must be greater than zero"));
        if (CapacityPerFrame > std::numeric_limits<Uint64>::max() / FramesInFlight)
            return std::unexpected(ErrorMessage("VulkanTransientUniformArena::Create: total buffer size overflow"));

        auto Buffer = VulkanHostBuffer::Create(
            CapacityPerFrame * FramesInFlight, vk::BufferUsageFlagBits::eUniformBuffer, Dev, Alloc);
        if (!Buffer)
            return std::unexpected(
                Buffer.error().Append("VulkanTransientUniformArena::Create: VulkanHostBuffer creation failed"));
        return VulkanTransientUniformArena(std::move(*Buffer), CapacityPerFrame, FramesInFlight);
    }

    VulkanTransientUniformArena(const VulkanTransientUniformArena&)                        = delete;
    auto operator=(const VulkanTransientUniformArena&) -> VulkanTransientUniformArena&     = delete;
    VulkanTransientUniformArena(VulkanTransientUniformArena&&) noexcept                    = default;
    auto operator=(VulkanTransientUniformArena&&) noexcept -> VulkanTransientUniformArena& = default;

    [[nodiscard]] auto BeginFrame(Uint32 FrameIndex) -> std::expected<void, ErrorMessage> {
        if (FrameIndex >= m_NextOffsets.size())
            return std::unexpected(
                ErrorMessage("VulkanTransientUniformArena::BeginFrame: frame index is out of range"));
        m_NextOffsets[FrameIndex] = static_cast<Uint64>(FrameIndex) * m_FrameCapacity;
        return {};
    }

    [[nodiscard]] auto Allocate(Uint32 FrameIndex, Uint64 Size) -> std::expected<Uint32, ErrorMessage> {
        if (FrameIndex >= m_NextOffsets.size())
            return std::unexpected(ErrorMessage("VulkanTransientUniformArena::Allocate: frame index is out of range"));
        if (Size == 0)
            return std::unexpected(
                ErrorMessage("VulkanTransientUniformArena::Allocate: size must be greater than zero"));

        const auto MaxRange = static_cast<Uint64>(VulkanCapability::Get().GetProperties().limits.maxUniformBufferRange);
        if (Size > MaxRange) {
            return std::unexpected(ErrorMessage(
                Format("VulkanTransientUniformArena allocation exceeds maxUniformBufferRange ({} bytes > {})",
                       Size,
                       MaxRange)));
        }

        const auto Alignment =
            static_cast<Uint64>(VulkanCapability::Get().GetProperties().limits.minUniformBufferOffsetAlignment);
        auto Offset = AlignUp(m_NextOffsets[FrameIndex], Alignment);
        if (!Offset)
            return std::unexpected(
                Offset.error().Append("VulkanTransientUniformArena::Allocate: offset alignment failed"));
        if (*Offset > std::numeric_limits<Uint64>::max() - Size)
            return std::unexpected(ErrorMessage("VulkanTransientUniformArena allocation size overflow"));

        const auto FrameEnd = (static_cast<Uint64>(FrameIndex) + 1) * m_FrameCapacity;
        if (*Offset + Size > FrameEnd) {
            return std::unexpected(ErrorMessage(Format(
                "VulkanTransientUniformArena allocation exceeds frame capacity (offset {} + size {} > frame end {})",
                *Offset,
                Size,
                FrameEnd)));
        }
        if (*Offset > std::numeric_limits<Uint32>::max())
            return std::unexpected(ErrorMessage("VulkanTransientUniformArena offset exceeds dynamic offset range"));

        m_NextOffsets[FrameIndex] = *Offset + Size;
        return static_cast<Uint32>(*Offset);
    }

    [[nodiscard]] auto Write(const void* Data, Uint64 Size, Uint32 Offset) -> std::expected<void, ErrorMessage> {
        if (auto R = m_Buffer.Upload(Data, Size, Offset); !R)
            return std::unexpected(R.error().Append("VulkanTransientUniformArena::Write failed"));
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
            return std::unexpected(ErrorMessage("VulkanTransientUniformArena alignment overflow"));
        return ((Value + Alignment - 1) / Alignment) * Alignment;
    }

    VulkanHostBuffer    m_Buffer        = {};
    Uint64              m_FrameCapacity = 0;
    std::vector<Uint64> m_NextOffsets   = {};
};

/// Shared per-frame storage-buffer arena for shader data rebuilt during command execution.
class VulkanTransientShaderStorageArena final {
  public:
    VulkanTransientShaderStorageArena() = default;

    explicit VulkanTransientShaderStorageArena(VulkanHostBuffer&& Buffer, Uint64 FrameCapacity, Uint32 FramesInFlight)
        : m_Buffer(std::move(Buffer)), m_FrameCapacity(FrameCapacity), m_NextOffsets(FramesInFlight) {}

    [[nodiscard]] static auto
    Create(Uint64 CapacityPerFrame, vk::raii::Device& Dev, VmaAllocator Alloc, Uint32 FramesInFlight)
        -> std::expected<VulkanTransientShaderStorageArena, ErrorMessage> {
        if (CapacityPerFrame == 0)
            return std::unexpected(
                ErrorMessage("VulkanTransientShaderStorageArena::Create: capacity must be greater than zero"));
        if (FramesInFlight == 0)
            return std::unexpected(
                ErrorMessage("VulkanTransientShaderStorageArena::Create: frames-in-flight must be greater than zero"));
        if (CapacityPerFrame > std::numeric_limits<Uint64>::max() / FramesInFlight)
            return std::unexpected(
                ErrorMessage("VulkanTransientShaderStorageArena::Create: total buffer size overflow"));

        auto Buffer = VulkanHostBuffer::Create(
            CapacityPerFrame * FramesInFlight,
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer,
            Dev,
            Alloc);
        if (!Buffer) {
            return std::unexpected(
                Buffer.error().Append("VulkanTransientShaderStorageArena::Create: VulkanHostBuffer creation failed"));
        }
        return VulkanTransientShaderStorageArena(std::move(*Buffer), CapacityPerFrame, FramesInFlight);
    }

    VulkanTransientShaderStorageArena(const VulkanTransientShaderStorageArena&)                        = delete;
    auto operator=(const VulkanTransientShaderStorageArena&) -> VulkanTransientShaderStorageArena&     = delete;
    VulkanTransientShaderStorageArena(VulkanTransientShaderStorageArena&&) noexcept                    = default;
    auto operator=(VulkanTransientShaderStorageArena&&) noexcept -> VulkanTransientShaderStorageArena& = default;

    [[nodiscard]] auto BeginFrame(Uint32 FrameIndex) -> std::expected<void, ErrorMessage> {
        if (FrameIndex >= m_NextOffsets.size()) {
            return std::unexpected(
                ErrorMessage("VulkanTransientShaderStorageArena::BeginFrame: frame index is out of range"));
        }
        m_NextOffsets[FrameIndex] = static_cast<Uint64>(FrameIndex) * m_FrameCapacity;
        return {};
    }

    [[nodiscard]] auto Allocate(Uint32 FrameIndex, Uint64 Size) -> std::expected<Uint64, ErrorMessage> {
        if (FrameIndex >= m_NextOffsets.size()) {
            return std::unexpected(
                ErrorMessage("VulkanTransientShaderStorageArena::Allocate: frame index is out of range"));
        }
        if (Size == 0)
            return std::unexpected(
                ErrorMessage("VulkanTransientShaderStorageArena::Allocate: size must be greater than zero"));

        const auto MaxRange = static_cast<Uint64>(VulkanCapability::Get().GetProperties().limits.maxStorageBufferRange);
        if (Size > MaxRange) {
            return std::unexpected(ErrorMessage(
                Format("VulkanTransientShaderStorageArena allocation exceeds maxStorageBufferRange ({} bytes > {})",
                       Size,
                       MaxRange)));
        }

        const auto Alignment =
            static_cast<Uint64>(VulkanCapability::Get().GetProperties().limits.minStorageBufferOffsetAlignment);
        auto Offset = AlignUp(m_NextOffsets[FrameIndex], Alignment);
        if (!Offset) {
            return std::unexpected(
                Offset.error().Append("VulkanTransientShaderStorageArena::Allocate: offset alignment failed"));
        }
        if (*Offset > std::numeric_limits<Uint64>::max() - Size)
            return std::unexpected(ErrorMessage("VulkanTransientShaderStorageArena allocation size overflow"));

        const auto FrameEnd = (static_cast<Uint64>(FrameIndex) + 1) * m_FrameCapacity;
        if (*Offset + Size > FrameEnd) {
            return std::unexpected(ErrorMessage(Format("VulkanTransientShaderStorageArena allocation exceeds frame "
                                                       "capacity (offset {} + size {} > frame end {})",
                                                       *Offset,
                                                       Size,
                                                       FrameEnd)));
        }

        m_NextOffsets[FrameIndex] = *Offset + Size;
        return *Offset;
    }

    [[nodiscard]] auto Write(const void* Data, Uint64 Size, Uint64 Offset) -> std::expected<void, ErrorMessage> {
        if (auto R = m_Buffer.Upload(Data, Size, Offset); !R)
            return std::unexpected(R.error().Append("VulkanTransientShaderStorageArena::Write failed"));
        return {};
    }

    [[nodiscard]] auto GetVkBuffer() const -> vk::Buffer {
        return m_Buffer.Get();
    }

  private:
    [[nodiscard]] static auto AlignUp(Uint64 Value, Uint64 Alignment) -> std::expected<Uint64, ErrorMessage> {
        if (Alignment == 0)
            return Value;
        if (Value > std::numeric_limits<Uint64>::max() - (Alignment - 1)) {
            return std::unexpected(ErrorMessage("VulkanTransientShaderStorageArena alignment overflow"));
        }
        return ((Value + Alignment - 1) / Alignment) * Alignment;
    }

    VulkanHostBuffer    m_Buffer        = {};
    Uint64              m_FrameCapacity = 0;
    std::vector<Uint64> m_NextOffsets   = {};
};

} // namespace SoulEngine
