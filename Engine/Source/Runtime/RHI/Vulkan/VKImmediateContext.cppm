module;

export module Vulkan:ImmediateContext;

import vulkan;
import std;
import RHI;
import :TransferCompletionQueue;

namespace SoulEngine {

/// One-shot GPU command executor (transfer queue).
///
/// Allocates transient eOneTimeSubmit command buffers, records via
/// caller-provided callback, submits to transfer queue with the provided
/// signal semaphore, returns immediately (no waitIdle).
///
/// RHICommand buffers are moved into a deferred-deletion callback so they
/// are automatically freed once the transfer timeline value is reached.
///
/// Thread-safety: not thread-safe, caller must serialise.
class VulkanImmediateContext {
  public:
    using CmdFn = std::function<void(const vk::raii::CommandBuffer&)>;

    VulkanImmediateContext() = default;

    [[nodiscard]] static auto Create(vk::raii::Device& Device,
                                      vk::raii::Queue& SubmitQueue,
                                      Uint32 SubmitQueueFamily,
                                      vk::PipelineStageFlagBits2 CompletionStage,
                                      VulkanTransferCompletionQueue& CompletionQueue)
        -> std::expected<VulkanImmediateContext, ErrorMessage> {
        vk::CommandPoolCreateInfo PoolCI{
            // Transient: hint driver that cmdbufs recorded and re-recorded often
            .flags = vk::CommandPoolCreateFlagBits::eTransient,
            .queueFamilyIndex = SubmitQueueFamily,
        };
        auto PoolRes = Device.createCommandPool(PoolCI);
        if (PoolRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Failed to create immediate command pool"));

        return VulkanImmediateContext(Device, SubmitQueue, std::move(PoolRes.value), CompletionStage, CompletionQueue);
    }

    /// Allocate one-shot cmdbuf, record via RecordFn, submit to transfer
    /// queue, return immediately (no waitIdle).
    ///
    /// The command buffer is freed via VulkanTransferCompletionQueue once the
    /// timeline reaches the returned token. Caller must keep all referenced
    /// resources alive until that point.
    [[nodiscard]] auto SubmitTransfer(const CmdFn& RecordFn) -> std::expected<RHIGpuCompletionToken, ErrorMessage> {
        auto CmdRes = m_Device->allocateCommandBuffers(vk::CommandBufferAllocateInfo{
            .commandPool        = *m_Pool,
            .level              = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        });
        if (CmdRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Failed to allocate immediate command buffer"));

        auto CmdBuf = std::move(CmdRes.value[0]);

        if (auto R = CmdBuf.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
            R != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Failed to begin immediate command buffer"));

        RecordFn(CmdBuf);

        if (auto R = CmdBuf.end(); R != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Failed to end immediate command buffer"));

        vk::CommandBufferSubmitInfo CmdBufInfo{.commandBuffer = CmdBuf};
        auto [Token, SignalSema] =
            m_CompletionQueue->AllocateSignalSubmitInfo(m_CompletionStage);
        vk::SemaphoreSubmitInfo     SignalSemas[] = {SignalSema};
        if (auto R = m_SubmitQueue->submit2(
                vk::SubmitInfo2{
                    .commandBufferInfoCount   = 1,
                    .pCommandBufferInfos      = &CmdBufInfo,
                    .signalSemaphoreInfoCount = 1,
                    .pSignalSemaphoreInfos    = SignalSemas,
                });
            R != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Failed to submit immediate command buffer"));

        // Move cmdbuf into a shared_ptr so the lambda is copyable for std::function
        auto CmdBufPtr = std::make_shared<vk::raii::CommandBuffer>(std::move(CmdBuf));
        m_CompletionQueue->EnqueueCallback(Token, [CmdBufPtr]() {
            // raii dtor frees cmdbuf safely here — GPU is done
        });

        return Token;
    }

    VulkanImmediateContext(VulkanImmediateContext&&)            = default;
    auto operator=(VulkanImmediateContext&&) -> VulkanImmediateContext& = default;

    VulkanImmediateContext(const VulkanImmediateContext&)            = delete;
    auto operator=(const VulkanImmediateContext&) -> VulkanImmediateContext& = delete;

  private:
    VulkanImmediateContext(vk::raii::Device& Device,
                      vk::raii::Queue& SubmitQueue,
                      vk::raii::CommandPool&& Pool,
                      vk::PipelineStageFlagBits2 CompletionStage,
                      VulkanTransferCompletionQueue& CompletionQueue)
        : m_Device(&Device)
        , m_SubmitQueue(&SubmitQueue)
        , m_Pool(std::move(Pool))
        , m_CompletionStage(CompletionStage)
        , m_CompletionQueue(&CompletionQueue) {}

    vk::raii::Device*            m_Device           = nullptr;
    vk::raii::Queue*             m_SubmitQueue      = nullptr;
    vk::raii::CommandPool        m_Pool             = nullptr;
    vk::PipelineStageFlagBits2   m_CompletionStage  = vk::PipelineStageFlagBits2::eAllCommands;
    VulkanTransferCompletionQueue*     m_CompletionQueue  = nullptr;
};

} // namespace SoulEngine
