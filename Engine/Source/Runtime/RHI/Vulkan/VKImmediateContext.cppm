module;

#include <vk_mem_alloc.h>

export module Vulkan:ImmediateContext;

import Core;
import RHI;
import vulkan;
import std;

import :Semaphore;

namespace SoulEngine {

/// Unified executor for short-lived transfer and graphics work.
///
/// Each logical immediate queue owns a command pool and a timeline semaphore.
/// Submit returns a queue-qualified timeline value that completes the task.
/// Dependencies between distinct logical queues use those timeline semaphores;
/// submissions to the same logical queue are ordered by its native VkQueue and
/// therefore need no explicit semaphore wait. The context is RHI-thread-only.
class VulkanImmediateContext {
  public:
    using CmdFn = std::function<void(const vk::raii::CommandBuffer&)>;

    /// A dependency produced by another logical immediate queue.
    /// WaitStage identifies the first consumer stage in this submission.
    struct WaitDependency {
        RHIGpuCompletionToken Token = {};
        vk::PipelineStageFlags2 WaitStage = vk::PipelineStageFlagBits2::eAllCommands;
    };

    VulkanImmediateContext() = default;

    [[nodiscard]] static auto Create(vk::raii::Device& Device,
                                     vk::raii::Queue& TransferQueue,
                                     Uint32 TransferFamily,
                                     vk::raii::Queue& GraphicsQueue,
                                     Uint32 GraphicsFamily)
        -> std::expected<VulkanImmediateContext, ErrorMessage> {
        VulkanImmediateContext Context;
        Context.m_Device = &Device;
        // These are distinct logical immediate queues even if both roles use
        // the same native VkQueue. They retain separate timeline token domains.
        if (auto R = Context.InitializeQueue(RHIImmediateQueue::Transfer, TransferQueue, TransferFamily); !R)
            return std::unexpected(R.error().Append("VulkanImmediateContext transfer queue initialization failed"));
        if (auto R = Context.InitializeQueue(RHIImmediateQueue::Graphics, GraphicsQueue, GraphicsFamily); !R)
            return std::unexpected(R.error().Append("VulkanImmediateContext graphics queue initialization failed"));
        return Context;
    }

    /// Record a one-time command buffer and submit it to Queue.
    ///
    /// SignalStage must include this task's last producer stage. Waits from the
    /// same logical queue are omitted because native VkQueue submission order
    /// already supplies that dependency.
    [[nodiscard]] auto Submit(RHIImmediateQueue Queue,
                              std::span<const WaitDependency> Waits,
                              vk::PipelineStageFlags2 SignalStage,
                              const CmdFn& RecordFn) -> std::expected<RHIGpuCompletionToken, ErrorMessage> {
        auto* State = GetState(Queue);
        if (!State)
            return std::unexpected(ErrorMessage("Immediate task requested an unavailable queue"));

        auto CmdRes = m_Device->allocateCommandBuffers(vk::CommandBufferAllocateInfo{
            .commandPool        = *State->Pool,
            .level              = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        });
        if (CmdRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Immediate task command-buffer allocation failed"));
        auto CmdBuf = std::move(CmdRes.value[0]);
        if (auto R = CmdBuf.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
            R != vk::Result::eSuccess) {
            return std::unexpected(ErrorMessage("Immediate task command-buffer begin failed"));
        }
        RecordFn(CmdBuf);
        if (auto R = CmdBuf.end(); R != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Immediate task command-buffer end failed"));

        // Cross-logical-queue dependencies use the producer's timeline
        // semaphore. Same-logical-queue dependencies use VkQueue submission
        // order and need no explicit wait.
        std::vector<vk::SemaphoreSubmitInfo> WaitInfos = {};
        WaitInfos.reserve(Waits.size());
        for (const auto& Wait : Waits) {
            if (Wait.Token.Value == 0)
                continue;
            auto* WaitState = GetState(Wait.Token.Queue);
            if (!WaitState)
                return std::unexpected(ErrorMessage("Immediate task wait references an unavailable queue"));
            if (Wait.Token.Queue == Queue)
                continue;
            WaitInfos.emplace_back(vk::SemaphoreSubmitInfo{
                .semaphore = WaitState->Timeline.Get(),
                .value     = Wait.Token.Value,
                .stageMask = Wait.WaitStage,
            });
        }

        const RHIGpuCompletionToken Token{.Queue = Queue, .Value = State->Timeline.NextValue()};
        const vk::SemaphoreSubmitInfo SignalInfo{
            .semaphore = State->Timeline.Get(),
            .value     = Token.Value,
            .stageMask = SignalStage,
        };
        const vk::CommandBufferSubmitInfo CmdInfo{.commandBuffer = CmdBuf};
        const vk::SubmitInfo2 SubmitInfo{
            .waitSemaphoreInfoCount   = static_cast<Uint32>(WaitInfos.size()),
            .pWaitSemaphoreInfos      = WaitInfos.data(),
            .commandBufferInfoCount   = 1,
            .pCommandBufferInfos      = &CmdInfo,
            .signalSemaphoreInfoCount = 1,
            .pSignalSemaphoreInfos    = &SignalInfo,
        };
        if (auto R = State->Queue->submit2({SubmitInfo}); R != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Immediate task submit failed"));

        State->LastSubmitted = Token.Value;
        // Keep the command buffer alive until its submission is complete; the
        // empty callback owns the RAII command-buffer wrapper for that period.
        EnqueueCompletionCallback(Token, [CommandBuffer = std::make_shared<vk::raii::CommandBuffer>(std::move(CmdBuf))]() {});
        return Token;
    }

    /// Poll whether Token has completed without retiring deferred callbacks.
    [[nodiscard]] auto IsComplete(RHIGpuCompletionToken Token) -> bool {
        if (Token.Value == 0)
            return true;
        auto* State = GetState(Token.Queue);
        if (!State)
            return false;
        auto Current = State->Timeline.GetCurrentValue();
        return Current && *Current >= Token.Value;
    }

    /// Retire Callback on Tick after Token reaches its timeline value.
    auto EnqueueCompletionCallback(RHIGpuCompletionToken Token, std::function<void()> Callback) -> void {
        auto* State = GetState(Token.Queue);
        if (!State || Token.Value == 0)
            return;
        State->Callbacks.emplace_back(PendingCallback{.Value = Token.Value, .Callback = std::move(Callback)});
    }

    /// Release completed command buffers and run their deferred callbacks.
    /// Called once per frame; IsComplete remains usable between ticks.
    auto Tick() -> void {
        TickState(m_Transfer);
        TickState(m_Graphics);
    }

    /// Block the CPU until Token completes. Use only where CPU visibility is required.
    [[nodiscard]] auto Wait(RHIGpuCompletionToken Token) -> std::expected<void, ErrorMessage> {
        if (Token.Value == 0)
            return {};
        auto* State = GetState(Token.Queue);
        if (!State)
            return std::unexpected(ErrorMessage("Immediate task wait references an unavailable queue"));
        return State->Timeline.Wait(Token.Value);
    }

    /// Wait for every submitted task during device shutdown, then retire all callbacks.
    [[nodiscard]] auto Drain() -> std::expected<void, ErrorMessage> {
        for (auto* State : {&m_Transfer, &m_Graphics}) {
            if (!State->Queue || State->LastSubmitted == 0)
                continue;
            if (auto R = State->Timeline.Wait(State->LastSubmitted); !R)
                return std::unexpected(R.error().Append("Immediate task timeline drain failed"));
        }
        Tick();
        return {};
    }

    VulkanImmediateContext(VulkanImmediateContext&&) = default;
    auto operator=(VulkanImmediateContext&&) -> VulkanImmediateContext& = default;
    VulkanImmediateContext(const VulkanImmediateContext&) = delete;
    auto operator=(const VulkanImmediateContext&) -> VulkanImmediateContext& = delete;

  private:
    /// A lifetime action retired once its logical immediate queue reaches Value.
    struct PendingCallback {
        Uint64 Value = 0;
        std::function<void()> Callback = {};
    };

    /// State isolated per logical immediate queue.
    ///
    /// Queue is the native VkQueue used for submission. Timelines remain
    /// independent even when two logical queues share that native VkQueue, so
    /// queue-qualified completion tokens cannot be confused.
    struct QueueState {
        vk::raii::Queue* Queue = nullptr;
        vk::raii::CommandPool Pool = nullptr;
        VulkanTimelineSemaphore Timeline;
        Uint64 LastSubmitted = 0;
        std::deque<PendingCallback> Callbacks = {};
    };

    [[nodiscard]] auto InitializeQueue(RHIImmediateQueue Kind, vk::raii::Queue& Queue, Uint32 Family)
        -> std::expected<void, ErrorMessage> {
        auto* State = GetState(Kind);
        if (!State)
            return std::unexpected(ErrorMessage("Immediate queue kind is unsupported"));
        // The transient pool belongs to the family executing this logical
        // queue's command buffers. A transfer-only family cannot execute
        // graphics or fragment pipeline stages.
        auto Timeline = VulkanTimelineSemaphore::Create(*m_Device);
        if (!Timeline)
            return std::unexpected(Timeline.error());
        auto PoolRes = m_Device->createCommandPool(vk::CommandPoolCreateInfo{
            .flags = vk::CommandPoolCreateFlagBits::eTransient,
            .queueFamilyIndex = Family,
        });
        if (PoolRes.result != vk::Result::eSuccess)
            return std::unexpected(ErrorMessage("Immediate task command-pool creation failed"));
        State->Queue = &Queue;
        State->Pool = std::move(PoolRes.value);
        State->Timeline = std::move(*Timeline);
        return {};
    }

    [[nodiscard]] auto GetState(RHIImmediateQueue Queue) -> QueueState* {
        switch (Queue) {
        case RHIImmediateQueue::Transfer: return &m_Transfer;
        case RHIImmediateQueue::Graphics: return &m_Graphics;
        case RHIImmediateQueue::Unknown:
        case RHIImmediateQueue::Compute: return nullptr;
        }
        return nullptr;
    }

    auto TickState(QueueState& State) -> void {
        if (!State.Queue || State.Callbacks.empty())
            return;
        // Callbacks are append-only in timeline submission order.
        auto Current = State.Timeline.GetCurrentValue();
        if (!Current)
            return;
        while (!State.Callbacks.empty() && State.Callbacks.front().Value <= *Current) {
            State.Callbacks.front().Callback();
            State.Callbacks.pop_front();
        }
    }

    vk::raii::Device* m_Device = nullptr;
    QueueState m_Transfer = {};
    QueueState m_Graphics = {};
};

} // namespace SoulEngine
