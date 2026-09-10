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
import :Debug;
import :Semaphore;

namespace SoulEngine {

// ═════════════════════════════════════════════════════════════════════════════
// VulkanHostBuffer — mappable staging buffer
// ═════════════════════════════════════════════════════════════════════════════

/// Internal mappable buffer for staging uploads.
/// Created via static Create(). Move-only.
class VulkanHostBuffer : public RHIObject {
  public:
    explicit VulkanHostBuffer(String Name) : RHIObject(std::move(Name)) {}

    [[nodiscard]] static auto
    Create(const VulkanResourceContext& Context, StringView Name, Uint64 Size, vk::BufferUsageFlags Usage)
        -> std::expected<VulkanHostBuffer, ErrorMessage> {
        VulkanHostBuffer Buf{String(Name)};
        Buf.m_Allocator = Context.GetAllocator();
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

        if (vmaCreateBuffer(Context.GetAllocator(),
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
            Buf.m_DeviceAddress    = Context.GetDevice().getBufferAddress(AddressInfo);
        }

        Context.GetDebugUtils().SetObjectName(Buf.m_Buffer, Buf.GetName());
        return Buf;
    }

    ~VulkanHostBuffer() {
        Destroy();
    }

    VulkanHostBuffer(VulkanHostBuffer&& Other) noexcept
        : RHIObject(std::move(Other)),
          m_Allocator(Other.m_Allocator),
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
            RHIObject::operator=(std::move(Other));
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
class VulkanDeviceBuffer : public RHIObject {
  public:
    explicit VulkanDeviceBuffer(String Name) : RHIObject(std::move(Name)) {}

    [[nodiscard]] static auto
    Create(const VulkanResourceContext& Context, StringView Name, Uint64 Size, vk::BufferUsageFlags Usage)
        -> std::expected<VulkanDeviceBuffer, ErrorMessage> {
        VulkanDeviceBuffer Buf{String(Name)};
        Buf.m_Allocator = Context.GetAllocator();
        const std::array QueueFamilies{Context.GetGraphicsFamily(), Context.GetTransferFamily()};
        const std::span  SharingFamilies = Context.GetGraphicsFamily() != Context.GetTransferFamily()
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

        if (vmaCreateBuffer(Context.GetAllocator(),
                            reinterpret_cast<VkBufferCreateInfo*>(&BufCI),
                            &AllocInfo,
                            reinterpret_cast<VkBuffer*>(&Buf.m_Buffer),
                            &Buf.m_Allocation,
                            nullptr) != VK_SUCCESS) {
            return std::unexpected(ErrorMessage("Failed to create VulkanDeviceBuffer via VMA"));
        }

        if (bBufferDeviceAddress) {
            const auto AddressInfo = vk::BufferDeviceAddressInfo{.buffer = Buf.m_Buffer};
            Buf.m_DeviceAddress    = Context.GetDevice().getBufferAddress(AddressInfo);
        }

        Context.GetDebugUtils().SetObjectName(Buf.m_Buffer, Buf.GetName());
        return Buf;
    }

    ~VulkanDeviceBuffer() {
        Destroy();
    }

    VulkanDeviceBuffer(VulkanDeviceBuffer&& Other) noexcept
        : RHIObject(std::move(Other)),
          m_Allocator(Other.m_Allocator),
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
            RHIObject::operator=(std::move(Other));
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
    [[nodiscard]] auto
    CopyFrom(VulkanHostBuffer& Src, VulkanImmediateContext& Ctx, VulkanImmediateContext::CompletionDesc Completion)
        -> std::expected<void, ErrorMessage> {
        const Uint64 CopySize = std::min(Src.GetSize(), m_Size);
        if (auto R = Ctx.Submit(
                VulkanImmediateQueue::Transfer,
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
    VulkanVertexBuffer(String Name, SPtr<VulkanDeviceBuffer> Buf, const RHIVertexBufferDesc& Desc)
        : RHIVertexBuffer(std::move(Name), Desc), m_Buffer(std::move(Buf)) {
        m_DescriptorInfo = vk::DescriptorBufferInfo{
            .buffer = GetVkBuffer(),
            .offset = 0,
            .range  = m_Buffer->GetSize(),
        };
    }

    ~VulkanVertexBuffer() override = default;

    /// Static factory: creates staging buffer, uploads data, copies to
    /// device-local buffer via VulkanImmediateContext, and defers staging destruction
    /// to VulkanTransferCompletionQueue.
    [[nodiscard]] static auto Create(const VulkanResourceContext&         Context,
                                     StringView                           Name,
                                     const RHIVertexBufferDesc&           Desc,
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

        auto StagingRes =
            VulkanHostBuffer::Create(Context, Format("{}#Staging", Name), Size, vk::BufferUsageFlagBits::eTransferSrc);
        if (!StagingRes)
            return std::unexpected(StagingRes.error().Append("VulkanVertexBuffer::Create: staging creation failed"));
        auto Staging = std::make_shared<VulkanHostBuffer>(std::move(*StagingRes));
        if (auto R = Staging->Upload(Desc.Data.data(), Desc.Data.size_bytes()); !R)
            return std::unexpected(R.error().Append("VulkanVertexBuffer::Create: staging upload failed"));

        auto Usage = vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
                     vk::BufferUsageFlagBits::eTransferDst;
        if (VulkanCapability::Get().IsRayTracingAvailable())
            Usage |= vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;
        auto DevRes = VulkanDeviceBuffer::Create(Context, Format("{}#Device", Name), Size, Usage);
        if (!DevRes)
            return std::unexpected(DevRes.error().Append("VulkanVertexBuffer::Create: device buffer creation failed"));
        auto Buffer     = std::make_shared<VulkanDeviceBuffer>(std::move(*DevRes));
        auto Completion = VulkanImmediateContext::CompletionDesc{
            .ConsumerQueue = VulkanImmediateQueue::Graphics,
            .OnComplete =
                [Staging, Buffer, OnReady = std::move(OnReady)]() mutable {
                    if (OnReady)
                        OnReady();
                },
        };
        if (auto R = Buffer->CopyFrom(*Staging, Context.GetImmediateContext(), std::move(Completion)); !R)
            return std::unexpected(R.error().Append("VulkanVertexBuffer::Create: staging copy failed"));

        return std::make_unique<VulkanVertexBuffer>(String(Name), std::move(Buffer), Desc);
    }
    [[nodiscard]] auto GetVkBuffer() const -> vk::Buffer {
        return m_Buffer->Get();
    }
    /// Build a storage-buffer descriptor write for this vertex buffer.
    [[nodiscard]] auto GetWriteDescriptorSet(vk::DescriptorSet Set,
                                             Uint32            BindingIndex,
                                             bool              /*IsReadOnly*/) const
        -> vk::WriteDescriptorSet {
        return vk::WriteDescriptorSet{
            .dstSet          = Set,
            .dstBinding      = BindingIndex,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType  = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo     = &m_DescriptorInfo,
        };
    }
    [[nodiscard]] auto GetDeviceAddress() const noexcept -> Uint64 override {
        return static_cast<Uint64>(m_Buffer->GetDeviceAddress());
    }
    VulkanVertexBuffer(const VulkanVertexBuffer&)                    = delete;
    auto operator=(const VulkanVertexBuffer&) -> VulkanVertexBuffer& = delete;
    VulkanVertexBuffer(VulkanVertexBuffer&&)                         = delete;
    auto operator=(VulkanVertexBuffer&&) -> VulkanVertexBuffer&      = delete;

  private:
    SPtr<VulkanDeviceBuffer>       m_Buffer        = nullptr;
    vk::DescriptorBufferInfo         m_DescriptorInfo = {};
};

// ═════════════════════════════════════════════════════════════════════════════
// VulkanIndexBuffer — typed index buffer
// ═════════════════════════════════════════════════════════════════════════════

class VulkanIndexBuffer final : public RHIIndexBuffer {
  public:
    VulkanIndexBuffer(String Name, SPtr<VulkanDeviceBuffer> Buf, const RHIIndexBufferDesc& Desc)
        : RHIIndexBuffer(std::move(Name), Desc), m_Buffer(std::move(Buf)) {
        m_DescriptorInfo = vk::DescriptorBufferInfo{
            .buffer = GetVkBuffer(),
            .offset = 0,
            .range  = m_Buffer->GetSize(),
        };
    }

    ~VulkanIndexBuffer() override = default;

    /// Static factory: same pattern as VulkanVertexBuffer::Create.
    /// Index type is hardcoded to uint32 (eUint32).  uint16 is not supported.
    [[nodiscard]] static auto Create(const VulkanResourceContext&         Context,
                                     StringView                           Name,
                                     const RHIIndexBufferDesc&            Desc,
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

        auto StagingRes =
            VulkanHostBuffer::Create(Context, Format("{}#Staging", Name), Size, vk::BufferUsageFlagBits::eTransferSrc);
        if (!StagingRes)
            return std::unexpected(StagingRes.error().Append("VulkanIndexBuffer::Create: staging creation failed"));
        auto Staging = std::make_shared<VulkanHostBuffer>(std::move(*StagingRes));
        if (auto R = Staging->Upload(Desc.Data.data(), Desc.Data.size_bytes()); !R)
            return std::unexpected(R.error().Append("VulkanIndexBuffer::Create: staging upload failed"));

        auto Usage = vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
                     vk::BufferUsageFlagBits::eTransferDst;
        if (VulkanCapability::Get().IsRayTracingAvailable())
            Usage |= vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;
        auto DevRes = VulkanDeviceBuffer::Create(Context, Format("{}#Device", Name), Size, Usage);
        if (!DevRes)
            return std::unexpected(DevRes.error().Append("VulkanIndexBuffer::Create: device buffer creation failed"));
        auto Buffer     = std::make_shared<VulkanDeviceBuffer>(std::move(*DevRes));
        auto Completion = VulkanImmediateContext::CompletionDesc{
            .ConsumerQueue = VulkanImmediateQueue::Graphics,
            .OnComplete =
                [Staging, Buffer, OnReady = std::move(OnReady)]() mutable {
                    if (OnReady)
                        OnReady();
                },
        };
        if (auto R = Buffer->CopyFrom(*Staging, Context.GetImmediateContext(), std::move(Completion)); !R)
            return std::unexpected(R.error().Append("VulkanIndexBuffer::Create: staging copy failed"));

        return std::make_unique<VulkanIndexBuffer>(String(Name), std::move(Buffer), Desc);
    }
    [[nodiscard]] auto GetVkBuffer() const -> vk::Buffer {
        return m_Buffer->Get();
    }
    /// Build a storage-buffer descriptor write for this index buffer.
    [[nodiscard]] auto GetWriteDescriptorSet(vk::DescriptorSet Set,
                                             Uint32            BindingIndex,
                                             bool              /*IsReadOnly*/) const
        -> vk::WriteDescriptorSet {
        return vk::WriteDescriptorSet{
            .dstSet          = Set,
            .dstBinding      = BindingIndex,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType  = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo     = &m_DescriptorInfo,
        };
    }
    [[nodiscard]] auto GetDeviceAddress() const noexcept -> Uint64 override {
        return static_cast<Uint64>(m_Buffer->GetDeviceAddress());
    }
    VulkanIndexBuffer(const VulkanIndexBuffer&)                    = delete;
    auto operator=(const VulkanIndexBuffer&) -> VulkanIndexBuffer& = delete;
    VulkanIndexBuffer(VulkanIndexBuffer&&)                         = delete;
    auto operator=(VulkanIndexBuffer&&) -> VulkanIndexBuffer&      = delete;

  private:
    SPtr<VulkanDeviceBuffer>       m_Buffer        = nullptr;
    vk::DescriptorBufferInfo         m_DescriptorInfo = {};
};

/// Shared per-frame uniform-buffer arena for values that must remain distinct
/// between individual draw commands.
class VulkanTransientUniformArena final {
  public:
    VulkanTransientUniformArena() = default;

    explicit VulkanTransientUniformArena(VulkanHostBuffer&& Buffer, Uint64 FrameCapacity, Uint32 FramesInFlight)
        : m_Buffer(std::move(Buffer)), m_FrameCapacity(FrameCapacity), m_FramesInFlight(FramesInFlight) {}

    [[nodiscard]] static auto
    Create(StringView Name, Uint64 CapacityPerFrame, const VulkanResourceContext& Context, Uint32 FramesInFlight)
        -> std::expected<VulkanTransientUniformArena, ErrorMessage> {
        if (CapacityPerFrame == 0)
            return std::unexpected(
                ErrorMessage("VulkanTransientUniformArena::Create: capacity must be greater than zero"));
        if (FramesInFlight == 0)
            return std::unexpected(
                ErrorMessage("VulkanTransientUniformArena::Create: frames-in-flight must be greater than zero"));
        const auto MaxRange = static_cast<Uint64>(VulkanCapability::Get().GetProperties().limits.maxUniformBufferRange);
        if (CapacityPerFrame > MaxRange)
            return std::unexpected(ErrorMessage(
                Format("VulkanTransientUniformArena::Create: capacity exceeds maxUniformBufferRange ({} bytes > {})",
                       CapacityPerFrame,
                       MaxRange)));
        const auto Alignment =
            static_cast<Uint64>(VulkanCapability::Get().GetProperties().limits.minUniformBufferOffsetAlignment);
        if (Alignment == 0 || CapacityPerFrame % Alignment != 0)
            return std::unexpected(ErrorMessage(Format(
                "VulkanTransientUniformArena::Create: capacity must be aligned to minUniformBufferOffsetAlignment "
                "({} bytes, alignment {})",
                CapacityPerFrame,
                Alignment)));
        const Uint64 PaddedFrameCount = static_cast<Uint64>(FramesInFlight) + 1;
        if (CapacityPerFrame > std::numeric_limits<Uint64>::max() / PaddedFrameCount)
            return std::unexpected(ErrorMessage("VulkanTransientUniformArena::Create: padded buffer size overflow"));

        // Keep one extra frame-capacity as trailing padding. The padding is
        // outside the frame regions used for uploads, so it does not change
        // rendering results. It is intentional memory waste that avoids the
        // per-frame descriptor-set complexity of dynamic binding updates and
        // prevents validation errors for a fixed range plus absolute offsets.
        // See: https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/2846
        auto Buffer = VulkanHostBuffer::Create(
            Context, String(Name), CapacityPerFrame * PaddedFrameCount, vk::BufferUsageFlagBits::eUniformBuffer);
        if (!Buffer)
            return std::unexpected(
                Buffer.error().Append("VulkanTransientUniformArena::Create: VulkanHostBuffer creation failed"));
        return VulkanTransientUniformArena(std::move(*Buffer), CapacityPerFrame, FramesInFlight);
    }

    VulkanTransientUniformArena(const VulkanTransientUniformArena&)                        = delete;
    auto operator=(const VulkanTransientUniformArena&) -> VulkanTransientUniformArena&     = delete;
    VulkanTransientUniformArena(VulkanTransientUniformArena&&) noexcept                    = default;
    auto operator=(VulkanTransientUniformArena&&) noexcept -> VulkanTransientUniformArena& = default;

    [[nodiscard]] auto Reset(Uint32 FrameIndex) -> std::expected<void, ErrorMessage> {
        if (FrameIndex >= m_FramesInFlight)
            return std::unexpected(
                ErrorMessage("VulkanTransientUniformArena::Reset: frame index is out of range"));
        m_FrameIndex = FrameIndex;
        m_Used       = 0;
        return {};
    }

    /// Reserve `Size` bytes in the current frame slab without writing host memory.
    [[nodiscard]] auto Allocate(Uint64 Size) -> std::expected<Uint32, ErrorMessage> {
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
        auto RelativeOffset = AlignUp(m_Used, Alignment);
        if (!RelativeOffset)
            return std::unexpected(
                RelativeOffset.error().Append("VulkanTransientUniformArena::Allocate: offset alignment failed"));
        if (*RelativeOffset > m_FrameCapacity || Size > m_FrameCapacity - *RelativeOffset)
            return std::unexpected(ErrorMessage(Format(
                "VulkanTransientUniformArena allocation exceeds frame capacity (offset {} + size {} > capacity {})",
                *RelativeOffset,
                Size,
                m_FrameCapacity)));
        const auto FrameBase = static_cast<Uint64>(m_FrameIndex) * m_FrameCapacity;
        if (FrameBase > std::numeric_limits<Uint64>::max() - *RelativeOffset)
            return std::unexpected(ErrorMessage("VulkanTransientUniformArena allocation offset overflow"));
        const auto Offset = FrameBase + *RelativeOffset;
        if (Offset > std::numeric_limits<Uint64>::max() - Size)
            return std::unexpected(ErrorMessage("VulkanTransientUniformArena allocation size overflow"));
        if (Offset > std::numeric_limits<Uint32>::max())
            return std::unexpected(ErrorMessage("VulkanTransientUniformArena offset exceeds dynamic offset range"));

        m_Used = *RelativeOffset + Size;
        return static_cast<Uint32>(Offset);
    }

    [[nodiscard]] auto Upload(std::span<const std::byte> Data) -> std::expected<Uint32, ErrorMessage> {
        const auto Size = static_cast<Uint64>(Data.size());
        auto Offset = Allocate(Size);
        if (!Offset)
            return std::unexpected(Offset.error().Append("VulkanTransientUniformArena::Upload: allocate failed"));
        if (auto R = m_Buffer.Upload(Data.data(), Size, *Offset); !R)
            return std::unexpected(R.error().Append("VulkanTransientUniformArena::Upload failed"));
        return *Offset;
    }

    [[nodiscard]] auto GetVkBuffer() const -> vk::Buffer {
        return m_Buffer.Get();
    }

    [[nodiscard]] auto GetFrameCapacity() const -> Uint64 {
        return m_FrameCapacity;
    }

  private:
    [[nodiscard]] static auto AlignUp(Uint64 Value, Uint64 Alignment) -> std::expected<Uint64, ErrorMessage> {
        if (Alignment == 0)
            return Value;
        if (Value > std::numeric_limits<Uint64>::max() - (Alignment - 1))
            return std::unexpected(ErrorMessage("VulkanTransientUniformArena alignment overflow"));
        return ((Value + Alignment - 1) / Alignment) * Alignment;
    }

    VulkanHostBuffer    m_Buffer        = VulkanHostBuffer{String{}};
    Uint64              m_FrameCapacity = 0;
    Uint32              m_FramesInFlight = 0;
    Uint32              m_FrameIndex     = 0;
    Uint64              m_Used           = 0;
};

/// Shared per-frame storage-buffer arena for shader data rebuilt during command execution.
class VulkanTransientShaderStorageArena final {
  public:
    VulkanTransientShaderStorageArena() = default;

    explicit VulkanTransientShaderStorageArena(VulkanHostBuffer&& Buffer, Uint64 FrameCapacity, Uint32 FramesInFlight)
        : m_Buffer(std::move(Buffer)), m_FrameCapacity(FrameCapacity), m_FramesInFlight(FramesInFlight) {}

    [[nodiscard]] static auto
    Create(StringView Name, Uint64 CapacityPerFrame, const VulkanResourceContext& Context, Uint32 FramesInFlight)
        -> std::expected<VulkanTransientShaderStorageArena, ErrorMessage> {
        if (CapacityPerFrame == 0)
            return std::unexpected(
                ErrorMessage("VulkanTransientShaderStorageArena::Create: capacity must be greater than zero"));
        if (FramesInFlight == 0)
            return std::unexpected(
                ErrorMessage("VulkanTransientShaderStorageArena::Create: frames-in-flight must be greater than zero"));
        const auto MaxRange =
            static_cast<Uint64>(VulkanCapability::Get().GetProperties().limits.maxStorageBufferRange);
        if (CapacityPerFrame > MaxRange)
            return std::unexpected(ErrorMessage(Format(
                "VulkanTransientShaderStorageArena::Create: capacity exceeds maxStorageBufferRange ({} bytes > {})",
                CapacityPerFrame,
                MaxRange)));
        const auto Alignment =
            static_cast<Uint64>(VulkanCapability::Get().GetProperties().limits.minStorageBufferOffsetAlignment);
        if (Alignment == 0 || CapacityPerFrame % Alignment != 0)
            return std::unexpected(ErrorMessage(Format(
                "VulkanTransientShaderStorageArena::Create: capacity must be aligned to "
                "minStorageBufferOffsetAlignment ({} bytes, alignment {})",
                CapacityPerFrame,
                Alignment)));
        const Uint64 PaddedFrameCount = static_cast<Uint64>(FramesInFlight) + 1;
        if (CapacityPerFrame > std::numeric_limits<Uint64>::max() / PaddedFrameCount)
            return std::unexpected(ErrorMessage("VulkanTransientShaderStorageArena::Create: padded buffer size overflow"));

        // Keep one extra frame-capacity as trailing padding. The padding is
        // outside the frame regions used for uploads, so it does not change
        // rendering results. It is intentional memory waste that avoids the
        // per-frame descriptor-set complexity of dynamic binding updates and
        // prevents validation errors for a fixed range plus absolute offsets.
        // See: https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/2846
        auto Buffer = VulkanHostBuffer::Create(Context,
                                               String(Name),
                                               CapacityPerFrame * PaddedFrameCount,
                                               vk::BufferUsageFlagBits::eStorageBuffer |
                                                   vk::BufferUsageFlagBits::eIndirectBuffer);
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

    [[nodiscard]] auto Reset(Uint32 FrameIndex) -> std::expected<void, ErrorMessage> {
        if (FrameIndex >= m_FramesInFlight) {
            return std::unexpected(
                ErrorMessage("VulkanTransientShaderStorageArena::Reset: frame index is out of range"));
        }
        m_FrameIndex = FrameIndex;
        m_Used       = 0;
        return {};
    }

    /// Reserve `Size` bytes in the current frame slab without writing host memory.
    [[nodiscard]] auto Allocate(Uint64 Size) -> std::expected<Uint32, ErrorMessage> {
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
        auto RelativeOffset = AlignUp(m_Used, Alignment);
        if (!RelativeOffset) {
            return std::unexpected(
                RelativeOffset.error().Append("VulkanTransientShaderStorageArena::Allocate: offset alignment failed"));
        }
        if (*RelativeOffset > m_FrameCapacity || Size > m_FrameCapacity - *RelativeOffset)
            return std::unexpected(ErrorMessage(Format(
                "VulkanTransientShaderStorageArena allocation exceeds frame capacity (offset {} + size {} > capacity {})",
                *RelativeOffset,
                Size,
                m_FrameCapacity)));
        const auto FrameBase = static_cast<Uint64>(m_FrameIndex) * m_FrameCapacity;
        if (FrameBase > std::numeric_limits<Uint64>::max() - *RelativeOffset)
            return std::unexpected(ErrorMessage("VulkanTransientShaderStorageArena allocation offset overflow"));
        const auto Offset = FrameBase + *RelativeOffset;
        if (Offset > std::numeric_limits<Uint64>::max() - Size)
            return std::unexpected(ErrorMessage("VulkanTransientShaderStorageArena allocation size overflow"));
        if (Offset > std::numeric_limits<Uint32>::max())
            return std::unexpected(ErrorMessage("VulkanTransientShaderStorageArena offset exceeds dynamic offset range"));

        m_Used = *RelativeOffset + Size;
        return static_cast<Uint32>(Offset);
    }

    [[nodiscard]] auto Upload(std::span<const std::byte> Data) -> std::expected<Uint32, ErrorMessage> {
        const auto Size = static_cast<Uint64>(Data.size());
        auto Offset = Allocate(Size);
        if (!Offset)
            return std::unexpected(
                Offset.error().Append("VulkanTransientShaderStorageArena::Upload: allocate failed"));
        if (auto R = m_Buffer.Upload(Data.data(), Size, *Offset); !R)
            return std::unexpected(R.error().Append("VulkanTransientShaderStorageArena::Upload failed"));
        return *Offset;
    }

    [[nodiscard]] auto GetVkBuffer() const -> vk::Buffer {
        return m_Buffer.Get();
    }

    [[nodiscard]] auto GetFrameCapacity() const -> Uint64 {
        return m_FrameCapacity;
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

    VulkanHostBuffer    m_Buffer        = VulkanHostBuffer{String{}};
    Uint64              m_FrameCapacity = 0;
    Uint32              m_FramesInFlight = 0;
    Uint32              m_FrameIndex     = 0;
    Uint64              m_Used           = 0;
};

class VulkanTransientConstantBuffer final : public RHITransientConstantBuffer {
  public:
    VulkanTransientConstantBuffer(String Name, Uint64 Size, Uint32 Offset, const VulkanTransientUniformArena& Arena)
        : RHITransientConstantBuffer(std::move(Name), Size),
          m_Offset(Offset),
          m_Arena(&Arena),
          m_DescriptorInfo{.buffer = Arena.GetVkBuffer(), .offset = 0, .range = Arena.GetFrameCapacity()} {}

    [[nodiscard]] auto GetOffset() const noexcept -> Uint32 {
        return m_Offset;
    }

    /// The arena buffer is the descriptor target; the per-frame slice offset is
    /// supplied as a dynamic offset at descriptor-set bind time.
    [[nodiscard]] auto GetArenaBuffer() const -> vk::Buffer {
        return m_Arena->GetVkBuffer();
    }

    /// Build a dynamic uniform-buffer descriptor write for the arena slice.
    [[nodiscard]] auto GetWriteDescriptorSet(vk::DescriptorSet Set,
                                             Uint32            BindingIndex,
                                             bool              /*IsReadOnly*/) const
        -> vk::WriteDescriptorSet {
        return vk::WriteDescriptorSet{
            .dstSet          = Set,
            .dstBinding      = BindingIndex,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType  = vk::DescriptorType::eUniformBufferDynamic,
            .pBufferInfo     = &m_DescriptorInfo,
        };
    }

    [[nodiscard]] auto GetArenaFrameCapacity() const -> Uint64 {
        return m_Arena->GetFrameCapacity();
    }

  private:
    Uint32                             m_Offset = 0;
    const VulkanTransientUniformArena* m_Arena = nullptr;
    vk::DescriptorBufferInfo            m_DescriptorInfo = {};
};

class VulkanTransientShaderStorageBuffer final : public RHITransientShaderStorageBuffer {
  public:
    VulkanTransientShaderStorageBuffer(String                             Name,
                                       Uint64                             Size,
                                       RHITransientBufferUsage            Usage,
                                       Uint32                             Offset,
                                       const VulkanTransientShaderStorageArena& Arena)
        : RHITransientShaderStorageBuffer(std::move(Name), Size, Usage),
          m_Offset(Offset),
          m_Arena(&Arena),
          m_DescriptorInfo{.buffer = Arena.GetVkBuffer(), .offset = 0, .range = Arena.GetFrameCapacity()} {}

    [[nodiscard]] auto GetOffset() const noexcept -> Uint32 {
        return m_Offset;
    }

    /// The arena buffer is the descriptor target; the per-frame slice offset is
    /// supplied as a dynamic offset at descriptor-set bind time.
    [[nodiscard]] auto GetArenaBuffer() const -> vk::Buffer {
        return m_Arena->GetVkBuffer();
    }

    /// Build a dynamic storage-buffer descriptor write for the arena slice.
    [[nodiscard]] auto GetWriteDescriptorSet(vk::DescriptorSet Set,
                                             Uint32            BindingIndex,
                                             bool              /*IsReadOnly*/) const
        -> vk::WriteDescriptorSet {
        return vk::WriteDescriptorSet{
            .dstSet          = Set,
            .dstBinding      = BindingIndex,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType  = vk::DescriptorType::eStorageBufferDynamic,
            .pBufferInfo     = &m_DescriptorInfo,
        };
    }

    [[nodiscard]] auto GetArenaFrameCapacity() const -> Uint64 {
        return m_Arena->GetFrameCapacity();
    }

  private:
    Uint32                                   m_Offset = 0;
    const VulkanTransientShaderStorageArena* m_Arena  = nullptr;
    vk::DescriptorBufferInfo                 m_DescriptorInfo = {};
};

// ═════════════════════════════════════════════════════════════════════════════
// VulkanReadbackBuffer — persistent host-visible GPU-to-CPU ring
// ═════════════════════════════════════════════════════════════════════════════

/// Persistent mapped ring buffer written by GPU transfer/compute work and
/// polled from the CPU. See RHIReadbackBuffer for the race protocol. The
/// completed-value query borrows the resource-context graphics timeline;
/// TryRead() is only valid while the resource context is alive.
class VulkanReadbackBuffer final : public RHIReadbackBuffer {
  public:
    VulkanReadbackBuffer(String                      Name,
                         const RHIReadbackBufferDesc& Desc,
                         VulkanTimelineSemaphore&     CompletedTimeline,
                         Uint32                       RingSize)
        : RHIReadbackBuffer(std::move(Name), Desc),
          m_CompletedTimeline(&CompletedTimeline),
          m_RingSize(RingSize),
          m_WriteValues(RingSize) {
        for (auto& Value : m_WriteValues)
            Value.store(0, std::memory_order_relaxed);
    }

    ~VulkanReadbackBuffer() override {
        if (m_Allocation)
            vmaDestroyBuffer(m_Allocator, static_cast<VkBuffer>(m_Buffer), m_Allocation);
    }
    VulkanReadbackBuffer(const VulkanReadbackBuffer&)                    = delete;
    auto operator=(const VulkanReadbackBuffer&) -> VulkanReadbackBuffer& = delete;
    VulkanReadbackBuffer(VulkanReadbackBuffer&&)                         = delete;
    auto operator=(VulkanReadbackBuffer&&) -> VulkanReadbackBuffer&      = delete;

    [[nodiscard]] static auto Create(const VulkanResourceContext& Context,
                                     StringView                   Name,
                                     const RHIReadbackBufferDesc& Desc,
                                     Uint32                       RingSize)
        -> std::expected<UPtr<VulkanReadbackBuffer>, ErrorMessage> {
        if (Desc.Size == 0 || RingSize == 0)
            return std::unexpected(ErrorMessage("VulkanReadbackBuffer::Create: size and ring size must be non-zero"));

        auto Result = std::make_unique<VulkanReadbackBuffer>(String(Name), Desc, Context.GetTimeline(), RingSize);
        Result->m_Allocator = Context.GetAllocator();

        const Uint64 Size = static_cast<Uint64>(Desc.Size) * RingSize;
        vk::BufferCreateInfo BufCI{
            .size        = Size,
            // eStorageBuffer keeps the buffer usable as a compute-dispatch
            // write target when the compute pipeline lands.
            .usage       = vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer,
            .sharingMode = vk::SharingMode::eExclusive,
        };

        VmaAllocationCreateInfo AllocInfo{
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };

        VmaAllocationInfo AllocationInfo{};
        if (vmaCreateBuffer(Context.GetAllocator(),
                            reinterpret_cast<VkBufferCreateInfo*>(&BufCI),
                            &AllocInfo,
                            reinterpret_cast<VkBuffer*>(&Result->m_Buffer),
                            &Result->m_Allocation,
                            &AllocationInfo) != VK_SUCCESS)
            return std::unexpected(ErrorMessage("VulkanReadbackBuffer::Create: VMA buffer creation failed"));
        if (!AllocationInfo.pMappedData) {
            vmaDestroyBuffer(Context.GetAllocator(),
                             static_cast<VkBuffer>(Result->m_Buffer),
                             Result->m_Allocation);
            Result->m_Buffer     = nullptr;
            Result->m_Allocation = nullptr;
            return std::unexpected(ErrorMessage("VulkanReadbackBuffer::Create: allocation is not mapped"));
        }
        Result->m_Mapped = static_cast<std::byte*>(AllocationInfo.pMappedData);

        VkMemoryPropertyFlags MemoryProps = 0;
        vmaGetAllocationMemoryProperties(Context.GetAllocator(), Result->m_Allocation, &MemoryProps);
        Result->m_Coherent = (MemoryProps & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

        Context.GetDebugUtils().SetObjectName(Result->m_Buffer, Result->GetName());
        return Result;
    }

    [[nodiscard]] auto GetVkBuffer() const -> vk::Buffer {
        return m_Buffer;
    }
    [[nodiscard]] auto AllocateWriteOffset() -> Uint64 {
        Uint32 OldestSlot  = 0;
        Uint64 OldestValue = m_WriteValues[0].load(std::memory_order_acquire);
        for (Uint32 Slot = 1; Slot < m_RingSize; ++Slot) {
            const Uint64 Value = m_WriteValues[Slot].load(std::memory_order_acquire);
            if (Value < OldestValue) {
                OldestSlot  = Slot;
                OldestValue = Value;
            }
        }
        m_WriteValues[OldestSlot].store(m_CompletedTimeline->IncreaseHostValue(), std::memory_order_release);
        return GetSlotOffset(OldestSlot);
    }
    [[nodiscard]] auto GetSlotOffset(Uint32 Slot) const -> Uint64 {
        return static_cast<Uint64>(Slot) * GetSize();
    }

  protected:
    [[nodiscard]] auto TryReadBytes(std::span<std::byte> Out) const -> bool override {
        const auto Completed = m_CompletedTimeline->GetDeviceValue();

        Uint64 BestValue = 0;
        Uint32 BestSlot  = std::numeric_limits<Uint32>::max();
        for (Uint32 Slot = 0; Slot < m_RingSize; ++Slot) {
            const Uint64 Value = m_WriteValues[Slot].load(std::memory_order_acquire);
            if (Value != 0 && Value <= Completed && Value > BestValue) {
                BestValue = Value;
                BestSlot  = Slot;
            }
        }
        if (BestSlot == std::numeric_limits<Uint32>::max())
            return false;

        const Uint64 Offset = GetSlotOffset(BestSlot);
        if (!m_Coherent)
            vmaInvalidateAllocation(m_Allocator, m_Allocation, Offset, Out.size());
        std::memcpy(Out.data(), m_Mapped + Offset, Out.size());
        return true;
    }

  private:
    VmaAllocator             m_Allocator         = nullptr;
    vk::Buffer               m_Buffer            = nullptr;
    VmaAllocation            m_Allocation        = nullptr;
    std::byte*               m_Mapped            = nullptr;
    VulkanTimelineSemaphore* m_CompletedTimeline = nullptr; // borrowed; device outlives this buffer
    Uint32                   m_RingSize          = 0;
    std::vector<std::atomic<Uint64>> m_WriteValues = {};
    bool                     m_Coherent          = false;
    static constexpr Uint64  kReservedWriteValue = std::numeric_limits<Uint64>::max();
};

} // namespace SoulEngine
