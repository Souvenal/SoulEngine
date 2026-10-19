module;

#include <vk_mem_alloc.h>

// magic_enum::enum_flags_test is not exported by the v0.9.8 module.
#include <magic_enum/magic_enum_flags.hpp>

module Vulkan:Buffer.impl;

import Core;
import TaskGraph;
import vulkan;
import RHI;
import std;

import :Buffer;
import :Capability;
import :Types;
import :Context;
import :Debug;
import :Semaphore;
import :RingBuffer;
import :Staging;

namespace SoulEngine {

auto VulkanUniformBelt::Create(const VulkanResourceContext& Context, StringView Name, Uint64 Capacity)
    -> std::expected<VulkanUniformBelt, ErrorMessage> {
    const auto Alignment =
        std::max(static_cast<Uint64>(VulkanCapability::Get().GetProperties().limits.minUniformBufferOffsetAlignment),
                 VulkanRingBuffer::kMinAlignment);
    auto Ring = VulkanRingBuffer::Create(Context, Name, vk::BufferUsageFlagBits::eUniformBuffer, Capacity, Alignment);
    if (!Ring)
        return std::unexpected(Ring.error().Append("VulkanUniformBelt: ring creation failed"));
    VulkanUniformBelt Belt;
    Belt.m_Ring = std::move(*Ring);
    return Belt;
}

auto VulkanUniformBelt::Allocate(Uint64 Size) -> std::expected<RingAllocation, ErrorMessage> {
    if (Size == 0)
        return std::unexpected(ErrorMessage("VulkanUniformBelt::Allocate: size must be greater than zero"));
    const auto MaxRange = static_cast<Uint64>(VulkanCapability::Get().GetProperties().limits.maxUniformBufferRange);
    if (Size > MaxRange)
        return std::unexpected(ErrorMessage(
            Format("VulkanUniformBelt allocation exceeds maxUniformBufferRange ({} bytes > {})", Size, MaxRange)));
    auto Alloc = m_Ring.Allocate(Size);
    if (!Alloc)
        return std::unexpected(Alloc.error().Append("VulkanUniformBelt::Allocate: ring allocation failed"));
    if (Alloc->Offset > std::numeric_limits<Uint32>::max())
        return std::unexpected(ErrorMessage("VulkanUniformBelt allocation offset exceeds dynamic offset range"));
    return *Alloc;
}

auto VulkanUniformBelt::Tick(Uint64 FrameBeginTimelineValue) -> void {
    ++m_Generation;
    m_Ring.Tick(FrameBeginTimelineValue);
}

auto VulkanUniformBelt::GetGeneration() const -> Uint32 {
    return m_Generation;
}

auto VulkanShaderStorageBelt::Create(const VulkanResourceContext& Context, StringView Name, Uint64 Capacity)
    -> std::expected<VulkanShaderStorageBelt, ErrorMessage> {
    const auto Alignment =
        std::max(static_cast<Uint64>(VulkanCapability::Get().GetProperties().limits.minStorageBufferOffsetAlignment),
                 VulkanRingBuffer::kMinAlignment);
    auto Ring =
        VulkanRingBuffer::Create(Context,
                                 Name,
                                 vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer,
                                 Capacity,
                                 Alignment);
    if (!Ring)
        return std::unexpected(Ring.error().Append("VulkanShaderStorageBelt: ring creation failed"));
    VulkanShaderStorageBelt Belt;
    Belt.m_Ring = std::move(*Ring);
    return Belt;
}

auto VulkanShaderStorageBelt::Allocate(Uint64 Size) -> std::expected<RingAllocation, ErrorMessage> {
    if (Size == 0)
        return std::unexpected(ErrorMessage("VulkanShaderStorageBelt::Allocate: size must be greater than zero"));
    const auto MaxRange = static_cast<Uint64>(VulkanCapability::Get().GetProperties().limits.maxStorageBufferRange);
    if (Size > MaxRange)
        return std::unexpected(ErrorMessage(Format(
            "VulkanShaderStorageBelt allocation exceeds maxStorageBufferRange ({} bytes > {})", Size, MaxRange)));
    auto Alloc = m_Ring.Allocate(Size);
    if (!Alloc)
        return std::unexpected(Alloc.error().Append("VulkanShaderStorageBelt::Allocate: ring allocation failed"));
    if (Alloc->Offset > std::numeric_limits<Uint32>::max())
        return std::unexpected(ErrorMessage("VulkanShaderStorageBelt allocation offset exceeds dynamic offset range"));
    return *Alloc;
}

auto VulkanShaderStorageBelt::Tick(Uint64 FrameBeginTimelineValue) -> void {
    ++m_Generation;
    m_Ring.Tick(FrameBeginTimelineValue);
}

auto VulkanShaderStorageBelt::GetGeneration() const -> Uint32 {
    return m_Generation;
}

auto ToVkBufferUsage(RHIBufferUsage Usage) -> vk::BufferUsageFlags {
    auto Out = vk::BufferUsageFlags{};
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::Vertex))
        Out |= vk::BufferUsageFlagBits::eVertexBuffer;
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::Index))
        Out |= vk::BufferUsageFlagBits::eIndexBuffer;
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::Constant))
        Out |= vk::BufferUsageFlagBits::eUniformBuffer;
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::Storage))
        Out |= vk::BufferUsageFlagBits::eStorageBuffer;
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::Indirect))
        Out |= vk::BufferUsageFlagBits::eIndirectBuffer;
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::TransferSrc))
        Out |= vk::BufferUsageFlagBits::eTransferSrc;
    if (magic_enum::enum_flags_test(Usage, RHIBufferUsage::TransferDst))
        Out |= vk::BufferUsageFlagBits::eTransferDst;
    // Geometry inputs feed acceleration-structure builds (old vertex/index classes did this unconditionally
    // when ray tracing is available).
    if ((magic_enum::enum_flags_test(Usage, RHIBufferUsage::Vertex) ||
         magic_enum::enum_flags_test(Usage, RHIBufferUsage::Index)) &&
        VulkanCapability::Get().IsRayTracingAvailable())
        Out |= vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;
    return Out;
}

VulkanBuffer::VulkanBuffer(String Name, const RHIBufferDesc& Desc, PersistentBufferProp Prop)
    : RHIBuffer(std::move(Name), Desc.SizeBytes, Desc.Usage, Desc.MemoryUsage, Desc.Lifetime) {
    m_Prop = std::move(Prop);
}

VulkanBuffer::VulkanBuffer(String Name, const RHIBufferDesc& Desc, TransientBufferProp Prop)
    : RHIBuffer(std::move(Name), Desc.SizeBytes, Desc.Usage, Desc.MemoryUsage, Desc.Lifetime) {
    m_Prop = std::move(Prop);
}

VulkanBuffer::~VulkanBuffer() {
    if (const auto* Prop = GetProp<PersistentBufferProp>(); Prop && Prop->Allocation)
        Prop->DeferredQueue->Enqueue(Prop->Buffer, Prop->Allocation);
}

auto VulkanBuffer::GetVkBuffer() const -> vk::Buffer {
    if (const auto* Prop = GetProp<TransientBufferProp>())
        return Prop->Allocation.Buffer;
    return std::get<PersistentBufferProp>(m_Prop).Buffer;
}

auto VulkanBuffer::GetDeviceAddress() const noexcept -> Uint64 {
    if (const auto* Prop = GetProp<PersistentBufferProp>())
        return static_cast<Uint64>(Prop->DeviceAddress);
    return 0;
}

auto VulkanBufferManager::Create(const VulkanResourceContext& Context,
                                 Uint64                       UniformBeltCapacity,
                                 Uint64                       StorageBeltCapacity,
                                 VulkanStagingBelt& Staging) -> std::expected<VulkanBufferManager, ErrorMessage> {
    VulkanBufferManager Manager;
    Manager.m_Context = &Context;
    auto Uniform      = VulkanUniformBelt::Create(Context, "Internal/Buffer/UniformBelt", UniformBeltCapacity);
    if (!Uniform)
        return std::unexpected(Uniform.error().Append("VulkanBufferManager: uniform belt creation failed"));
    Manager.m_UniformBelt = std::move(*Uniform);
    auto Storage = VulkanShaderStorageBelt::Create(Context, "Internal/Buffer/ShaderStorageBelt", StorageBeltCapacity);
    if (!Storage)
        return std::unexpected(Storage.error().Append("VulkanBufferManager: shader storage belt creation failed"));
    Manager.m_StorageBelt = std::move(*Storage);
    Manager.m_Staging     = &Staging;
    return Manager;
}

auto VulkanBufferManager::Shutdown() -> void {
    m_StorageBelt = {};
    m_UniformBelt = {};
    m_Staging     = nullptr;
    m_Context     = nullptr;
}

auto VulkanBufferManager::CreateBuffer(StringView Name, const RHIBufferDesc& Desc)
    -> std::expected<RHIRef<RHIBuffer>, ErrorMessage> {
    if (auto Valid = ValidateDesc(Desc); !Valid)
        return std::unexpected(Valid.error());
    if (Desc.Lifetime == RHILifetime::Transient)
        return CreateTransientBuffer(Name, Desc);
    return CreatePersistentBuffer(Name, Desc);
}

auto VulkanBufferManager::Upload(RHIRef<RHIBuffer> Buffer, std::span<const std::byte> Data)
    -> std::expected<void, ErrorMessage> {
    if (Data.empty())
        return std::unexpected(ErrorMessage("UploadBuffer: data is empty"));
    // Fail-fast validation when the payload already exists (persistent buffers are Ready at creation).
    // Transient slices materialize on the RHI thread, so their validation is deferred to the upload task —
    // the frame task queue is FIFO, so the creation task of a same-frame create+upload pair always runs
    // first.
    if (const auto* Payload = Buffer.TryGet()) {
        if (Data.size_bytes() != Payload->GetSize())
            return std::unexpected(ErrorMessage(Format(
                "UploadBuffer: data size {} does not match buffer size {}", Data.size_bytes(), Payload->GetSize())));
    }

    auto EnqueueResult = TaskGraph::Get().EnqueueFrameTask(
        ThreadQueue::RHI,
        [this, Buffer = std::move(Buffer), Snapshot = std::vector<std::byte>{Data.begin(), Data.end()}]() mutable {
            auto* TaskPayload = Buffer.TryGet();
            if (!TaskPayload) {
                LogWarning("UploadBuffer: buffer ref is not Ready when the upload task ran");
                return;
            }
            if (Snapshot.size() != TaskPayload->GetSize()) {
                LogWarning("UploadBuffer: data size {} does not match buffer size {}",
                           Snapshot.size(),
                           TaskPayload->GetSize());
                return;
            }
            auto& VulkanBuf = static_cast<VulkanBuffer&>(*TaskPayload);
            if (VulkanBuf.GetLifetime() == RHILifetime::Transient) {
                UploadTransientBuffer(VulkanBuf, Snapshot);
                return;
            }
            UploadPersistentBuffer(VulkanBuf, Snapshot);
        });
    if (!EnqueueResult)
        return std::unexpected(EnqueueResult.error().Append("UploadBuffer: frame task enqueue failed"));
    return {};
}

auto VulkanBufferManager::Tick(Uint64 FrameBeginTimelineValue) -> void {
    m_UniformBelt.Tick(FrameBeginTimelineValue);
    m_StorageBelt.Tick(FrameBeginTimelineValue);
}

auto VulkanBufferManager::UploadTransientBuffer(VulkanBuffer& Buffer, std::span<const std::byte> Data) -> void {
    const auto* Prop = Buffer.GetProp<TransientBufferProp>();
    if (!Prop) {
        LogWarning("UploadBuffer: buffer '{}' has no transient state", Buffer.GetName());
        return;
    }
    const bool   IsUniform  = magic_enum::enum_flags_test(Buffer.GetUsage(), RHIBufferUsage::Constant);
    const Uint32 Generation = IsUniform ? m_UniformBelt.GetGeneration() : m_StorageBelt.GetGeneration();
    if (Prop->CreationGeneration != Generation) {
        LogWarning("UploadBuffer: transient buffer '{}' belongs to a previous belt generation — "
                   "the upload is dropped",
                   Buffer.GetName());
        return;
    }
    std::memcpy(Prop->Allocation.MappedData, Data.data(), Data.size());
}

auto VulkanBufferManager::UploadPersistentBuffer(VulkanBuffer& Buffer, std::span<const std::byte> Data) -> void {
    const auto* Prop = Buffer.GetProp<PersistentBufferProp>();
    if (!Prop) {
        LogWarning("UploadBuffer: buffer '{}' has no persistent state", Buffer.GetName());
        return;
    }
    // Host-visible flavor: memory copies stay on the RHI thread; no staging, no frame-start batch entry.
    if (Buffer.GetMemoryUsage() == RHIMemoryUsage::CpuToGpu) {
        if (vmaCopyMemoryToAllocation(Prop->Allocator, Data.data(), Prop->Allocation, 0, Data.size()) != VK_SUCCESS)
            LogWarning("UploadBuffer: host write failed for '{}'", Buffer.GetName());
        return;
    }
    m_Staging->EnqueueBufferUpload(Buffer.GetName(), Buffer.GetVkBuffer(), Buffer.GetUsage(), Data);
}

auto VulkanBufferManager::ValidateDesc(const RHIBufferDesc& Desc) -> std::expected<void, ErrorMessage> {
    if (Desc.SizeBytes == 0)
        return std::unexpected(ErrorMessage("RHIBufferDesc: size is zero"));
    if (Desc.Usage == RHIBufferUsage::Unknown)
        return std::unexpected(ErrorMessage("RHIBufferDesc: usage is Unknown — a Desc is explicit"));
    if (Desc.Lifetime == RHILifetime::Unknown)
        return std::unexpected(ErrorMessage("RHIBufferDesc: lifetime is Unknown — a Desc is explicit"));
    if (Desc.Lifetime == RHILifetime::Transient) {
        // Transient slices live in the host-coherent belts — CpuToGpu is the only honest memory domain.
        // Pinning it keeps the axis explicit: no Unknown loophole, no silent GpuOnly lie.
        if (Desc.MemoryUsage != RHIMemoryUsage::CpuToGpu)
            return std::unexpected(
                ErrorMessage("RHIBufferDesc: transient buffers are belt slices — memory usage must be CpuToGpu"));
        // Only two belt lanes exist today: uniform (Constant) and shader storage (Storage). Indirect is an
        // orthogonal usage the storage ring already carries, not a lane of its own. "Not uniform" must never
        // silently mean "storage" — a usage with neither lane bit has no belt and is rejected.
        constexpr auto kLaneUsages = RHIBufferUsage::Constant | RHIBufferUsage::Storage;
        if (!magic_enum::enum_flags_test_any(Desc.Usage, kLaneUsages))
            return std::unexpected(
                ErrorMessage("RHIBufferDesc: transient buffers currently allow only the Constant (uniform belt) or "
                             "Storage (shader-storage belt) usage bits"));
        return {};
    }
    if (Desc.MemoryUsage == RHIMemoryUsage::Unknown)
        return std::unexpected(ErrorMessage("RHIBufferDesc: memory usage is Unknown — a Desc is explicit"));
    return {};
}

auto VulkanBufferManager::CreatePersistentBuffer(StringView Name, const RHIBufferDesc& Desc)
    -> std::expected<RHIRef<RHIBuffer>, ErrorMessage> {
    PersistentBufferProp Prop{.Allocator     = m_Context->GetAllocator(),
                              .DeferredQueue = &m_Context->GetDeferredDeletionQueue()};

    // GpuOnly derives transfer-dst (upload is a transfer by definition) and device address (BDA costs nothing
    // at creation) from placement.
    vk::BufferUsageFlags VkUsage = ToVkBufferUsage(Desc.Usage);
    const bool           bDeviceAddress =
        Desc.MemoryUsage == RHIMemoryUsage::GpuOnly &&
        static_cast<bool>(
            VulkanCapability::Get().GetFeatures<vk::PhysicalDeviceVulkan12Features>().bufferDeviceAddress);
    if (Desc.MemoryUsage == RHIMemoryUsage::GpuOnly) {
        VkUsage |= vk::BufferUsageFlagBits::eTransferDst;
        if (bDeviceAddress)
            VkUsage |= vk::BufferUsageFlagBits::eShaderDeviceAddress;
    }

    // GpuOnly uploads are recorded on the transfer lane while consumers run on graphics; cross-family
    // ownership rides an explicit queue-family transfer, so every buffer is created exclusive. TODO: emit the
    // transfer->graphics ownership transfer barriers.
    const vk::BufferCreateInfo BufCI{
        .size        = Desc.SizeBytes,
        .usage       = VkUsage,
        .sharingMode = vk::SharingMode::eExclusive,
    };

    // NOTE: VMA memory types are `AUTO`. To understand what `AUTO` means here, think about some concepts,
    //       like host/device visible and host/device memory.
    //
    //       Classic discrete graphics cards have no ReBAR, which means device memory is invisible to
    //       host, thus `host visible` == `host memory`. On the other hand, cards that have ReBAR own
    //       device memory that are visible to host.
    //
    //       GPU (or shader program) can access both device memory and host memory. The later one is much
    //       slower because it needs to went through PCIE. So:
    //       - For `GpuOnly` buffers, they prefer device memory, but host memory can do the work as well.
    //       - For `CpuToGpu` buffers, we need host visiblity. VMA `AUTO` does the work
    //         here: If device memory is visble to host, then it chooses device memory. If it isn't, we
    //         have to use host memory. (Why not use `PREFER_HOST`? Because we don't want host memory when
    //         device memory is visible to host)
    //
    //       We declare we need host visibilty by adding `HOST_ACCESS_xxx` bit in flags.
    const VmaAllocationCreateInfo AllocInfo =
        Desc.MemoryUsage == RHIMemoryUsage::GpuOnly
            ? VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE}
            : VmaAllocationCreateInfo{.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                                      .usage = VMA_MEMORY_USAGE_AUTO};

    const auto Created = vmaCreateBuffer(m_Context->GetAllocator(),
                                         reinterpret_cast<const VkBufferCreateInfo*>(&BufCI),
                                         &AllocInfo,
                                         reinterpret_cast<VkBuffer*>(&Prop.Buffer),
                                         &Prop.Allocation,
                                         nullptr);
    if (Created != VK_SUCCESS)
        return std::unexpected(
            ErrorMessage("VulkanBufferManager::CreatePersistentBuffer: buffer creation failed via VMA"));

    if (bDeviceAddress) {
        const auto AddressInfo = vk::BufferDeviceAddressInfo{.buffer = Prop.Buffer};
        Prop.DeviceAddress     = m_Context->GetDevice().getBufferAddress(AddressInfo);
    }

    m_Context->GetDebugUtils().SetObjectName(Prop.Buffer, Name);

    auto Resource = RHIRef<RHIBuffer>::Create();
    if (auto Publish =
            Resource.Publish(std::make_unique<VulkanBuffer>(String(Name), Desc, std::move(Prop)), RHIRefState::Ready);
        !Publish)
        return std::unexpected(Publish.error());
    return Resource;
}

auto VulkanBufferManager::CreateTransientBuffer(StringView Name, const RHIBufferDesc& Desc)
    -> std::expected<RHIRef<RHIBuffer>, ErrorMessage> {
    auto Resource = RHIRef<RHIBuffer>::Create();

    auto EnqueueResult =
        TaskGraph::Get().EnqueueFrameTask(ThreadQueue::RHI, [this, Resource, Name = String(Name), Desc] mutable {
            const bool IsUniform = magic_enum::enum_flags_test(Desc.Usage, RHIBufferUsage::Constant);
            auto Alloc = IsUniform ? m_UniformBelt.Allocate(Desc.SizeBytes) : m_StorageBelt.Allocate(Desc.SizeBytes);
            if (!Alloc) {
                auto Error = Alloc.error().Append("Transient buffer belt resolution failed");
                LogError("{}", Error.ToString());
                Resource.MarkFailed(std::move(Error));
                return;
            }
            const Uint32 Generation = IsUniform ? m_UniformBelt.GetGeneration() : m_StorageBelt.GetGeneration();
            // Desc was validated and pinned (CpuToGpu, Transient) at the CreateBuffer entry — forward it
            // as-is.
            auto         Payload    = std::make_unique<VulkanBuffer>(
                std::move(Name), Desc, TransientBufferProp{.Allocation = *Alloc, .CreationGeneration = Generation});
            if (auto Publish = Resource.Publish(std::move(Payload), RHIRefState::Ready); !Publish) {
                auto Error = Publish.error().Append("Transient buffer publication failed");
                LogError("{}", Error.ToString());
                Resource.MarkFailed(std::move(Error));
                return;
            }
        });
    if (!EnqueueResult) {
        Resource.MarkFailed(EnqueueResult.error());
        return std::unexpected(EnqueueResult.error());
    }
    return Resource;
}

} // namespace SoulEngine
