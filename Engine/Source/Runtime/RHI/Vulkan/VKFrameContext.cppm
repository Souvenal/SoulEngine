export module Vulkan:FrameContext;

import Core;
import RHI;
import vulkan;
import std;

import :Context;

namespace SoulEngine {

/// Per-frame-in-flight state: semaphores, timeline value, and the
/// command buffer for this frame slot — everything that cycles with
/// the frame index lives here.
///
/// Named VulkanFrameContext to reserve FrameData for the future game→render
/// thread transfer struct that will carry draw commands, view state, etc.
struct VulkanFrameContext {
    vk::raii::Semaphore                  PresentComplete                 = nullptr;
    Uint64                               SubmissionCompleteTimelineValue = 0;
    vk::raii::CommandPool                Pool                            = nullptr;
    vk::raii::CommandBuffer              PrimaryBuffer                   = nullptr;
    vk::raii::CommandPool                SubPool                         = nullptr;
    /// Per-frame scratch secondaries for RenderPassList execution.
    /// Allocated each frame in Execute(), freed in next frame's BeginFrame
    /// after timeline wait guarantees GPU has consumed them.
    std::vector<vk::raii::CommandBuffer> ScratchSecondaries;

    [[nodiscard]] static auto Create(const VulkanResourceContext& Context, Uint32 FrameIndex)
        -> std::expected<VulkanFrameContext, ErrorMessage> {
        auto SemaRes = Context.GetDevice().createSemaphore({});
        if (SemaRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Failed to create present-complete semaphore"));
        Context.GetDebugUtils().SetObjectName(*SemaRes.value, Format("Internal/Semaphore/PresentComplete/Frame{}", FrameIndex));

        // ── Main pool (for primary buffer) ───────────────────────────────
        vk::CommandPoolCreateInfo PoolCI{
            .flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = Context.GetGraphicsFamily(),
        };
        auto PoolRes = Context.GetDevice().createCommandPool(PoolCI);
        if (PoolRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("VulkanFrameContext: failed to create main command pool"));
        Context.GetDebugUtils().SetObjectName(*PoolRes.value, Format("Internal/CommandPool/Primary/Frame{}", FrameIndex));

        // Allocate one primary command buffer from pool.
        vk::CommandBufferAllocateInfo PrimaryAlloc{
            .commandPool        = *PoolRes.value,
            .level              = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        };
        auto PrimaryRes = Context.GetDevice().allocateCommandBuffers(PrimaryAlloc);
        if (PrimaryRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("VulkanFrameContext: failed to allocate primary command buffer"));
        Context.GetDebugUtils().SetObjectName(
            *PrimaryRes.value[0], Format("Internal/CommandBuffer/Primary/Frame{}", FrameIndex));

        // ── Sub pool (per RenderPassList secondary buffers) ────────────────
        vk::CommandPoolCreateInfo SubPoolCI{
            .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer | vk::CommandPoolCreateFlagBits::eTransient,
            .queueFamilyIndex = Context.GetGraphicsFamily(),
        };
        auto SubPoolRes = Context.GetDevice().createCommandPool(SubPoolCI);
        if (SubPoolRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("VulkanFrameContext: failed to create sub command pool"));
        Context.GetDebugUtils().SetObjectName(*SubPoolRes.value, Format("Internal/CommandPool/Secondary/Frame{}", FrameIndex));

        return VulkanFrameContext{
            .PresentComplete                 = std::move(SemaRes.value),
            .SubmissionCompleteTimelineValue = 0,
            .Pool                            = std::move(PoolRes.value),
            .PrimaryBuffer                   = std::move(PrimaryRes.value[0]),
            .SubPool                         = std::move(SubPoolRes.value),
        };
    }
};

} // namespace SoulEngine
