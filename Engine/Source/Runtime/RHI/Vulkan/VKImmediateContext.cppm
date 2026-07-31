module;

#include <vk_mem_alloc.h>

export module Vulkan:ImmediateContext;

import Core;
import RHI;
import vulkan;
import std;

import :Semaphore;

namespace SoulEngine {

/// Logical immediate-task queue owned solely by the Vulkan backend.
export enum class VulkanImmediateQueue : Uint8 {
    Unknown = 0,
    Transfer,
    Graphics,
};

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
    using CompletionFn = std::move_only_function<void()>;

    struct CompletionDesc {
        VulkanImmediateQueue ConsumerQueue = VulkanImmediateQueue::Unknown;
        CompletionFn         OnComplete    = {};
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
        if (auto R = Context.InitializeQueue(VulkanImmediateQueue::Transfer, TransferQueue, TransferFamily); !R)
            return std::unexpected(R.error().Append("VulkanImmediateContext transfer queue initialization failed"));
        if (auto R = Context.InitializeQueue(VulkanImmediateQueue::Graphics, GraphicsQueue, GraphicsFamily); !R)
            return std::unexpected(R.error().Append("VulkanImmediateContext graphics queue initialization failed"));
        return Context;
    }

    /// Record a one-time command buffer and submit it to Queue.
    ///
    /// SignalStage must include this task's last producer stage. Waits from the
    /// same logical queue are omitted because native VkQueue submission order
    /// already supplies that dependency.
    [[nodiscard]] auto Submit(VulkanImmediateQueue Queue,
                              vk::PipelineStageFlags2 SignalStage,
                              const CmdFn& RecordFn) -> std::expected<void, ErrorMessage> {
        return Submit(Queue, SignalStage, RecordFn, CompletionDesc{});
    }

    [[nodiscard]] auto Submit(VulkanImmediateQueue Queue,
                              vk::PipelineStageFlags2 SignalStage,
                              const CmdFn& RecordFn,
                              CompletionDesc Completion) -> std::expected<void, ErrorMessage> {
        auto Producer = SubmitRaw(Queue, {}, SignalStage, RecordFn);
        if (!Producer)
            return std::unexpected(Producer.error());

        if (!Completion.OnComplete)
            return {};

        const auto ConsumerQueue = Completion.ConsumerQueue == VulkanImmediateQueue::Unknown ? Queue : Completion.ConsumerQueue;
        if (ConsumerQueue == Queue) {
            EnqueueCompletionCallback(*Producer, std::move(Completion.OnComplete));
            return {};
        }

        const WaitDependency Wait{.Point = *Producer};
        auto Bridge = SubmitRaw(ConsumerQueue,
                                std::span<const WaitDependency>{&Wait, 1},
                                vk::PipelineStageFlagBits2::eAllCommands,
                                [](const vk::raii::CommandBuffer&) {});
        if (!Bridge) {
            // Once the producer submission succeeds, completion-owned captures
            // must remain alive until that GPU work retires even when the bridge
            // cannot be submitted. The callback's state transition is guarded
            // by RHIRefPayload::TryMarkReady().
            EnqueueCompletionCallback(*Producer, std::move(Completion.OnComplete));
            return std::unexpected(Bridge.error().Append("Immediate task consumer bridge submission failed"));
        }

        EnqueueCompletionCallback(*Bridge, std::move(Completion.OnComplete));
        return {};
    }

    [[nodiscard]] auto SubmitAndWait(VulkanImmediateQueue Queue,
                                     vk::PipelineStageFlags2 SignalStage,
                                     const CmdFn& RecordFn) -> std::expected<void, ErrorMessage> {
        auto Submission = SubmitRaw(Queue, {}, SignalStage, RecordFn);
        if (!Submission)
            return std::unexpected(Submission.error());
        auto* State = GetState(Queue);
        if (auto R = State->Timeline.Wait(Submission->Value); !R)
            return std::unexpected(R.error().Append("Immediate task CPU wait failed"));
        TickState(*State);
        return {};
    }

    /// Release completed command buffers and run their deferred callbacks.
    /// Called once per frame; IsComplete remains usable between ticks.
    auto Tick() -> void {
        TickState(m_Transfer);
        TickState(m_Graphics);
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
    struct TimelinePoint {
        VulkanImmediateQueue Queue = VulkanImmediateQueue::Unknown;
        Uint64               Value = 0;
    };

    /// A dependency produced by another logical immediate queue.
    /// WaitStage identifies the first consumer stage in this submission.
    struct WaitDependency {
        TimelinePoint               Point     = {};
        vk::PipelineStageFlags2 WaitStage = vk::PipelineStageFlagBits2::eAllCommands;
    };

    /// A lifetime action retired once its logical immediate queue reaches Value.
    struct PendingCallback {
        Uint64       Value    = 0;
        CompletionFn Callback = {};
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

    [[nodiscard]] auto SubmitRaw(VulkanImmediateQueue Queue,
                                 std::span<const WaitDependency> Waits,
                                 vk::PipelineStageFlags2 SignalStage,
                                 const CmdFn& RecordFn) -> std::expected<TimelinePoint, ErrorMessage> {
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
            if (Wait.Point.Value == 0)
                continue;
            auto* WaitState = GetState(Wait.Point.Queue);
            if (!WaitState)
                return std::unexpected(ErrorMessage("Immediate task wait references an unavailable queue"));
            if (Wait.Point.Queue == Queue)
                continue;
            WaitInfos.emplace_back(vk::SemaphoreSubmitInfo{
                .semaphore = WaitState->Timeline.Get(),
                .value     = Wait.Point.Value,
                .stageMask = Wait.WaitStage,
            });
        }

        const TimelinePoint Point{.Queue = Queue, .Value = State->Timeline.NextValue()};
        const vk::SemaphoreSubmitInfo SignalInfo{
            .semaphore = State->Timeline.Get(),
            .value     = Point.Value,
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

        State->LastSubmitted = Point.Value;
        // Keep the command buffer alive until its submission is complete; the
        // empty callback owns the RAII command-buffer wrapper for that period.
        EnqueueCompletionCallback(Point, [CommandBuffer = std::make_shared<vk::raii::CommandBuffer>(std::move(CmdBuf))]() {});
        return Point;
    }

    /// Poll whether Token has completed without retiring deferred callbacks.
    [[nodiscard]] auto IsComplete(TimelinePoint Point) -> bool {
        if (Point.Value == 0)
            return true;
        auto* State = GetState(Point.Queue);
        if (!State)
            return false;
        auto Current = State->Timeline.GetCurrentValue();
        return Current && *Current >= Point.Value;
    }

    /// Retire Callback on Tick after Token reaches its timeline value.
    auto EnqueueCompletionCallback(TimelinePoint Point, CompletionFn Callback) -> void {
        auto* State = GetState(Point.Queue);
        if (!State || Point.Value == 0)
            return;
        State->Callbacks.emplace_back(PendingCallback{.Value = Point.Value, .Callback = std::move(Callback)});
    }

    /// Block the CPU until Token completes. Use only where CPU visibility is required.
    [[nodiscard]] auto Wait(TimelinePoint Point) -> std::expected<void, ErrorMessage> {
        if (Point.Value == 0)
            return {};
        auto* State = GetState(Point.Queue);
        if (!State)
            return std::unexpected(ErrorMessage("Immediate task wait references an unavailable queue"));
        return State->Timeline.Wait(Point.Value);
    }

    [[nodiscard]] auto InitializeQueue(VulkanImmediateQueue Kind, vk::raii::Queue& Queue, Uint32 Family)
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

    [[nodiscard]] auto GetState(VulkanImmediateQueue Queue) -> QueueState* {
        switch (Queue) {
        case VulkanImmediateQueue::Transfer: return &m_Transfer;
        case VulkanImmediateQueue::Graphics: return &m_Graphics;
        case VulkanImmediateQueue::Unknown: return nullptr;
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
