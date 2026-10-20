export module RHIVulkan:FrameContext;

import Core;
import RHI;
import vulkan;
import std;

import :Context;

namespace SoulEngine {

/// @brief Per-frame-in-flight state: semaphores, timeline value, and the command buffer for this frame slot.
///
/// Everything that cycles with the frame index lives here.
///
/// Named VulkanFrameContext to reserve FrameData for the future game→render thread transfer struct that will
/// carry draw commands, view state, etc.
struct VulkanFrameContext {
    vk::raii::Semaphore                  PresentComplete                 = nullptr;
    Uint64                               SubmissionCompleteTimelineValue = 0;
    vk::raii::CommandPool                Pool                            = nullptr;
    vk::raii::CommandBuffer              PrimaryBuffer                   = nullptr;
    vk::raii::CommandPool                SubPool                         = nullptr;
    /// @brief Per-frame scratch secondaries for RenderPassList execution.
    ///
    /// Allocated each frame in Execute(), freed in next frame's BeginFrame after timeline wait guarantees GPU
    /// has consumed them.
    std::vector<vk::raii::CommandBuffer> ScratchSecondaries;

    [[nodiscard]] static auto Create(const VulkanResourceContext& Context, Uint32 FrameIndex)
        -> std::expected<VulkanFrameContext, ErrorMessage>;
};

} // namespace SoulEngine
