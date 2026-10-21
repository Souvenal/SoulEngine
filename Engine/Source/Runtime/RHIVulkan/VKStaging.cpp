module RHIVulkan.Staging;

import Core;
import vulkan;
import RHI;
import std;

import RHIVulkan.RingBuffer;
import RHIVulkan.Types;
import RHIVulkan.Context;
import RHIVulkan.Debug;
import RHIVulkan.Profiling;

namespace SoulEngine {

auto VulkanStagingBelt::Create(const VulkanResourceContext& Context, Uint64 InitialCapacity)
    -> std::expected<VulkanStagingBelt, ErrorMessage> {
    auto Ring = VulkanRingBuffer::Create(
        Context, "Internal/Buffer/StagingRing", vk::BufferUsageFlagBits::eTransferSrc, InitialCapacity);
    if (!Ring)
        return std::unexpected(Ring.error().Append("VulkanStagingBelt: staging ring creation failed"));
    VulkanStagingBelt Belt;
    Belt.m_Context = &Context;
    Belt.m_Ring    = std::move(*Ring);
    return Belt;
}

auto VulkanStagingBelt::EnqueueBufferUpload(StringView                 Name,
                                            vk::Buffer                 DstBuffer,
                                            RHIBufferUsage             Usage,
                                            std::span<const std::byte> Data) -> void {
    // The batch cannot know the first consumer's direction, so Access is pinned to Read: the upload contract
    // is that uploaded bytes are read first. A write-first consumer's WAW hazard rides the pass prologue, not
    // this barrier.
    EnqueueBufferUpload(Name, DstBuffer, ToVkBufferAccess(Usage, RHIAccess::Read), Data);
}

auto VulkanStagingBelt::EnqueueBufferUpload(StringView                 Name,
                                            vk::Buffer                 DstBuffer,
                                            vk::AccessFlags2           AccessAfter,
                                            std::span<const std::byte> Data) -> void {
    auto Alloc = m_Ring.Allocate(Data.size_bytes());
    if (!Alloc) {
        LogWarning("UploadBuffer: staging ring allocation failed for '{}': {}", Name, Alloc.error().ToString());
        return;
    }
    std::memcpy(Alloc->MappedData, Data.data(), Data.size_bytes());
    m_PendingBufferUploads.push_back(
        VulkanPendingBufferUpload{.Staging = *Alloc, .DstBuffer = DstBuffer, .AccessAfter = AccessAfter});
}

auto VulkanStagingBelt::EnqueueImageUpload(
    StringView Name, vk::Image DstImage, Uint32 Width, Uint32 Height, std::span<const std::byte> Data) -> void {
    auto Alloc = m_Ring.Allocate(Data.size_bytes());
    if (!Alloc) {
        LogWarning("UploadTexture: staging ring allocation failed for '{}': {}", Name, Alloc.error().ToString());
        return;
    }
    std::memcpy(Alloc->MappedData, Data.data(), Data.size_bytes());
    m_PendingImageUploads.push_back(
        VulkanPendingImageUpload{.Staging = *Alloc, .DstImage = DstImage, .Width = Width, .Height = Height});
}

auto VulkanStagingBelt::RecordUploadBatch(vk::raii::CommandBuffer& CmdBuffer) -> void {
    if (m_PendingBufferUploads.empty() && m_PendingImageUploads.empty())
        return;
    RecordBufferLane(CmdBuffer);
    RecordImageLane(CmdBuffer);
}

auto VulkanStagingBelt::RecordBufferLane(vk::raii::CommandBuffer& CmdBuffer) -> void {
    if (m_PendingBufferUploads.empty())
        return;
    const LabelScope BatchLabel{*m_Context, CmdBuffer, "BufferUploadBatch"};
    vk::AccessFlags2 AccessAfter = {};
    for (const auto& Entry : m_PendingBufferUploads) {
        const vk::BufferCopy2     Region{.srcOffset = Entry.Staging.Offset, .dstOffset = 0, .size = Entry.Staging.Size};
        const vk::CopyBufferInfo2 CopyInfo{
            .srcBuffer   = Entry.Staging.Buffer,
            .dstBuffer   = Entry.DstBuffer,
            .regionCount = 1,
            .pRegions    = &Region,
        };
        CmdBuffer.copyBuffer2(CopyInfo);
        AccessAfter |= Entry.AccessAfter;
    }
    if (AccessAfter) {
        // TODO: derive dstStageMask from the destination usage union instead of the blanket eAllCommands.
        const vk::MemoryBarrier2 Barrier{
            .srcStageMask  = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = AccessAfter,
        };
        const vk::DependencyInfo Dependency{.memoryBarrierCount = 1, .pMemoryBarriers = &Barrier};
        CmdBuffer.pipelineBarrier2(Dependency);
    }
    m_PendingBufferUploads.clear();
}

auto VulkanStagingBelt::RecordImageLane(vk::raii::CommandBuffer& CmdBuffer) -> void {
    if (m_PendingImageUploads.empty())
        return;
    const LabelScope                BatchLabel{*m_Context, CmdBuffer, "TextureUploadBatch"};
    const vk::ImageSubresourceRange Mip0Range{.aspectMask     = vk::ImageAspectFlagBits::eColor,
                                              .baseMipLevel   = 0,
                                              .levelCount     = 1,
                                              .baseArrayLayer = 0,
                                              .layerCount     = 1};
    for (const auto& Entry : m_PendingImageUploads) {
        m_Context->GetImageTracker().Transition(CmdBuffer,
                                                Entry.DstImage,
                                                Mip0Range,
                                                VulkanImageState{
                                                    .stage   = vk::PipelineStageFlagBits2::eTransfer,
                                                    .access  = vk::AccessFlagBits2::eTransferWrite,
                                                    .layout  = vk::ImageLayout::eTransferDstOptimal,
                                                    .isWrite = true,
                                                });
        const vk::BufferImageCopy Region{
            .bufferOffset      = Entry.Staging.Offset,
            .bufferRowLength   = 0,
            .bufferImageHeight = 0,
            .imageSubresource  = {.aspectMask     = vk::ImageAspectFlagBits::eColor,
                                  .mipLevel       = 0,
                                  .baseArrayLayer = 0,
                                  .layerCount     = 1},
            .imageOffset       = {0, 0, 0},
            .imageExtent       = {Entry.Width, Entry.Height, 1},
        };
        CmdBuffer.copyBufferToImage(
            Entry.Staging.Buffer, Entry.DstImage, vk::ImageLayout::eTransferDstOptimal, {Region});
        m_Context->GetImageTracker().Transition(CmdBuffer,
                                                Entry.DstImage,
                                                Mip0Range,
                                                VulkanImageState{
                                                    .stage  = vk::PipelineStageFlagBits2::eNone,
                                                    .layout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                                });
    }
    m_PendingImageUploads.clear();
}

auto VulkanStagingBelt::Tick(Uint64 FrameBeginTimelineValue) -> void {
    m_Ring.Tick(FrameBeginTimelineValue);
}

} // namespace SoulEngine
