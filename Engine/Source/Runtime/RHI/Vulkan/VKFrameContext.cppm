module;

#include <vk_mem_alloc.h>

export module Vulkan:FrameContext;

import Core;
import RHI;
import vulkan;
import std;

import :Buffer;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

/// Per-frame-in-flight state: semaphores, timeline value, and the
/// command buffer for this frame slot — everything that cycles with
/// the frame index lives here.
///
/// Named FrameContext to reserve FrameData for the future game→render
/// thread transfer struct that will carry draw commands, view state, etc.
struct FrameContext {
    vk::raii::Semaphore       PresentComplete                 = nullptr;
    Uint64                    SubmissionCompleteTimelineValue = 0;
    vk::raii::CommandPool     Pool                            = nullptr;
    vk::raii::CommandBuffer   PrimaryBuffer                   = nullptr;
    vk::raii::CommandPool     SubPool                         = nullptr;
    Core::SPtr<UniformBuffer> GlobalConstantBuffer            = nullptr;

    /// Per-frame scratch secondaries for RHICommandList execution.
    /// Allocated each frame in Execute(), freed in next frame's BeginFrame
    /// after timeline wait guarantees GPU has consumed them.
    std::vector<vk::raii::CommandBuffer> ScratchSecondaries;

    [[nodiscard]] static auto Create(vk::raii::Device& Device, Uint32 QueueFamily, VmaAllocator Allocator)
        -> std::expected<FrameContext, ErrorMessage> {
        auto SemaRes = Device.createSemaphore({});
        if (SemaRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Failed to create present-complete semaphore"));

        // ── Main pool (for primary buffer) ───────────────────────────────
        vk::CommandPoolCreateInfo PoolCI{
            .flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = QueueFamily,
        };
        auto PoolRes = Device.createCommandPool(PoolCI);
        if (PoolRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("FrameContext: failed to create main command pool"));

        // Allocate one primary command buffer from pool.
        vk::CommandBufferAllocateInfo PrimaryAlloc{
            .commandPool        = *PoolRes.value,
            .level              = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        };
        auto PrimaryRes = Device.allocateCommandBuffers(PrimaryAlloc);
        if (PrimaryRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("FrameContext: failed to allocate primary command buffer"));

        // ── Sub pool (per CommandList secondary buffers) ─────────────────
        vk::CommandPoolCreateInfo SubPoolCI{
            .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer | vk::CommandPoolCreateFlagBits::eTransient,
            .queueFamilyIndex = QueueFamily,
        };
        auto SubPoolRes = Device.createCommandPool(SubPoolCI);
        if (SubPoolRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("FrameContext: failed to create sub command pool"));

        // ── Constant buffer ──────────────────────────────────────────────
        const auto&             Cfg       = ConfigManager::Get().GetConfig();
        Uint64                  CBBufSize = Cfg.RhiVulkan.GlobalConstantBufferSize.value_or(256);
        RHI::ConstantBufferDesc CBDesc{.Size = CBBufSize};

        auto CB = UniformBuffer::Create(CBDesc, *Device, Allocator);
        if (!CB)
            return std::unexpected(CB.error().Append("FrameContext::Create: UniformBuffer creation failed"));

        return FrameContext{
            .PresentComplete                 = std::move(SemaRes.value),
            .SubmissionCompleteTimelineValue = 0,
            .Pool                            = std::move(PoolRes.value),
            .PrimaryBuffer                   = std::move(PrimaryRes.value[0]),
            .SubPool                         = std::move(SubPoolRes.value),
            .GlobalConstantBuffer            = std::move(*CB),
        };
    }
};

} // namespace SoulEngine::RHI::Vulkan
