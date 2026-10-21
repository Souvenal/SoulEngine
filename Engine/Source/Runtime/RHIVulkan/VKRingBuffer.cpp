module;

#include <vk_mem_alloc.h>

module RHIVulkan.RingBuffer;

import Core;
import vulkan;
import std;

import RHIVulkan.Context;
import RHIVulkan.Debug;

namespace SoulEngine {

auto VulkanRingBuffer::Create(const VulkanResourceContext& Context,
                              StringView                   Name,
                              vk::BufferUsageFlags         Usage,
                              Uint64                       Capacity,
                              Uint64                       Alignment,
                              VulkanRingMemoryClass MemoryClass) -> std::expected<VulkanRingBuffer, ErrorMessage> {
    if (!Usage)
        return std::unexpected(ErrorMessage("VulkanRingBuffer::Create: usage is empty"));
    if (Capacity == 0)
        return std::unexpected(ErrorMessage("VulkanRingBuffer::Create: capacity must be greater than zero"));
    if (Alignment < kMinAlignment || (Alignment & (Alignment - 1)) != 0)
        return std::unexpected(
            ErrorMessage(Format("VulkanRingBuffer::Create: alignment must be a power of two and at least {} (got {})",
                                kMinAlignment,
                                Alignment)));
    if (MemoryClass == VulkanRingMemoryClass::Unknown)
        return std::unexpected(ErrorMessage("VulkanRingBuffer::Create: memory class is Unknown"));
    VulkanRingBuffer Ring;
    Ring.m_Context     = &Context;
    Ring.m_Name        = String(Name);
    Ring.m_Usage       = Usage;
    Ring.m_Alignment   = Alignment;
    Ring.m_MemoryClass = MemoryClass;
    auto Instance      = Ring.CreateInstance(Capacity);
    if (!Instance)
        return std::unexpected(Instance.error().Append("VulkanRingBuffer::Create: initial buffer creation failed"));
    Ring.m_Current = std::move(*Instance);
    return Ring;
}

auto VulkanRingBuffer::Allocate(Uint64 Size) -> std::expected<RingAllocation, ErrorMessage> {
    if (Size == 0)
        return std::unexpected(ErrorMessage("VulkanRingBuffer::Allocate: size must be greater than zero"));
    if (Size > std::numeric_limits<Uint64>::max() - (m_Alignment - 1))
        return std::unexpected(ErrorMessage("VulkanRingBuffer::Allocate: size alignment overflow"));
    const Uint64 Aligned = (Size + m_Alignment - 1) / m_Alignment * m_Alignment;

    for (;;) {
        Instance&    I        = *m_Current;
        const Uint64 Capacity = I.Capacity;
        // Strict: a slice exactly as large as the instance could only be placed exactly full — divert it to
        // growth like any exact fit.
        if (Aligned < Capacity) {
            if (I.Head == I.Tail) {
                // Empty instance: head room first, then wrap to the front.
                if (Aligned <= Capacity - I.Head) {
                    if (auto S = TryPlace(I, Aligned, Size, I.Head))
                        return *S;
                } else if (auto S = TryPlace(I, Aligned, Size, 0)) {
                    return *S;
                }
            } else if (I.Head < I.Tail) {
                // Free space is the contiguous retired span ahead of the head.
                if (Aligned <= I.Tail - I.Head) {
                    if (auto S = TryPlace(I, Aligned, Size, I.Head))
                        return *S;
                }
            } else {
                // Head room first; otherwise wrap into the retired front span.
                if (Aligned <= Capacity - I.Head) {
                    if (auto S = TryPlace(I, Aligned, Size, I.Head))
                        return *S;
                }
                if (Aligned <= I.Tail) {
                    if (auto S = TryPlace(I, Aligned, Size, 0))
                        return *S;
                }
            }
        }
        // No placement fits: grow. The new capacity must be strictly larger than the request — an exact fit
        // would divert again. The superseded instance is stamped at the next frame-begin seal and destroyed
        // by Tick() once the device timeline passes the stamp.
        Uint64 NewCapacity = Aligned + m_Alignment;
        if (Capacity <= std::numeric_limits<Uint64>::max() / 2)
            NewCapacity = std::max(Capacity * 2, NewCapacity);
        auto Grown = CreateInstance(NewCapacity);
        if (!Grown)
            return std::unexpected(Grown.error().Append("VulkanRingBuffer::Allocate: growth allocation failed"));
        LogInfo("VulkanRingBuffer: growing from {} to {} bytes", Capacity, NewCapacity);
        m_InstancesToRetire.push_back(std::move(m_Current));
        m_Current = std::move(*Grown);
    }
}

auto VulkanRingBuffer::Tick(Uint64 FrameBeginTimelineValue) -> void {
    const Uint64 Completed = m_Context->GetTimeline().GetDeviceValue();

    // step 1: delete used-up instances
    std::erase_if(m_RetiredInstances, [Completed](const RetiredInstance& R) { return R.TimelineValue <= Completed; });

    if (!m_Current)
        return;
    Instance& I = *m_Current;

    // step 2: give back used-up ranges in current instance
    while (!I.Markers.empty() && I.Markers.front().TimelineValue <= Completed) {
        I.Tail = I.Markers.front().Offset;
        I.Markers.pop_front();
    }

    // step 3: seal the previous frame's pending allocations and superseded instances
    if (I.IsDirty) {
        I.Markers.push_back(RingMarker{.TimelineValue = FrameBeginTimelineValue, .Offset = I.Head});
        I.IsDirty = false;
    }
    for (auto& Inst : m_InstancesToRetire)
        m_RetiredInstances.push_back(
            RetiredInstance{.TimelineValue = FrameBeginTimelineValue, .Instance = std::move(Inst)});
    m_InstancesToRetire.clear();
}

VulkanRingBuffer::Instance::~Instance() {
    if (Allocation)
        vmaDestroyBuffer(Allocator, static_cast<VkBuffer>(Buffer), Allocation);
}

auto VulkanRingBuffer::CreateInstance(Uint64 Capacity) -> std::expected<UPtr<Instance>, ErrorMessage> {
    auto Out       = std::make_unique<Instance>();
    Out->Allocator = m_Context->GetAllocator();
    Out->Capacity  = Capacity;

    const bool                 IsHostMapped = m_MemoryClass == VulkanRingMemoryClass::HostMapped;
    const vk::BufferCreateInfo BufCI{
        .size        = Capacity,
        .usage       = m_Usage,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const VmaAllocationCreateInfo AllocInfo{
        .flags = IsHostMapped
                     ? static_cast<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                                             VMA_ALLOCATION_CREATE_MAPPED_BIT)
                     : static_cast<VmaAllocationCreateFlags>(0),
        .usage = IsHostMapped ? VMA_MEMORY_USAGE_AUTO : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    VmaAllocationInfo AllocationInfo{};
    if (vmaCreateBuffer(m_Context->GetAllocator(),
                        reinterpret_cast<const VkBufferCreateInfo*>(&BufCI),
                        &AllocInfo,
                        reinterpret_cast<VkBuffer*>(&Out->Buffer),
                        &Out->Allocation,
                        &AllocationInfo) != VK_SUCCESS)
        return std::unexpected(ErrorMessage("VulkanRingBuffer: VMA buffer creation failed"));
    if (IsHostMapped) {
        if (!AllocationInfo.pMappedData)
            return std::unexpected(ErrorMessage("VulkanRingBuffer: allocation is not persistently mapped"));
        Out->Mapped = static_cast<std::byte*>(AllocationInfo.pMappedData);

        // Contractual requirement: ring writes rely on implicit host-write visibility at submission — there
        // is no HOST barrier anywhere.
        VkMemoryPropertyFlags Props = 0;
        vmaGetAllocationMemoryProperties(m_Context->GetAllocator(), Out->Allocation, &Props);
        if ((Props & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
            return std::unexpected(ErrorMessage("VulkanRingBuffer: ring memory is not host-coherent"));
    }

    // Cache the base device address once: per-allocation address queries would be one device call per range.
    if (m_Usage & vk::BufferUsageFlagBits::eShaderDeviceAddress)
        Out->BaseAddress = m_Context->GetDevice().getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = Out->Buffer});

    m_Context->GetDebugUtils().SetObjectName(Out->Buffer, m_Name);
    return Out;
}

auto VulkanRingBuffer::TryPlace(Instance& I, Uint64 Aligned, Uint64 Size, Uint64 At) -> std::optional<RingAllocation> {
    const Uint64 NewHead = (At + Aligned) % I.Capacity;
    if (NewHead == I.Tail)
        return std::nullopt;
    const RingAllocation Result{.Buffer        = I.Buffer,
                                .MappedData    = I.Mapped ? I.Mapped + At : nullptr,
                                .Offset        = At,
                                .Size          = Size,
                                .DeviceAddress = I.BaseAddress == 0 ? 0 : I.BaseAddress + At};
    I.Head    = NewHead;
    I.IsDirty = true;
    return Result;
}

} // namespace SoulEngine
