module;

export module Vulkan:TransferCompletionQueue;

import Core;
import RHI;
import vulkan;
import std;

import :Semaphore;

namespace SoulEngine {

/// Transfer-completion queue backed by a dedicated timeline semaphore.
///
/// VulkanHostBuffer staging uploads, VulkanDeviceBuffer transfers, and other GPU resources
/// that must outlive their submitting queue operation are enqueued with a
/// timeline value. Tick() fires callbacks whose timeline value has been reached
/// (non-blocking, safe to call every frame).
///
/// Owns its own vk::SemaphoreType::eTimeline — separate from the frame-level
/// timeline in VulkanRenderDevice. This guarantees monotonic signal ordering on the
/// transfer queue without cross-queue ordering constraints.
class VulkanTransferCompletionQueue {
  public:
    VulkanTransferCompletionQueue() = default;

    VulkanTransferCompletionQueue(VulkanTransferCompletionQueue&&)                    = default;
    auto operator=(VulkanTransferCompletionQueue&&) -> VulkanTransferCompletionQueue& = default;

    VulkanTransferCompletionQueue(const VulkanTransferCompletionQueue&)                    = delete;
    auto operator=(const VulkanTransferCompletionQueue&) -> VulkanTransferCompletionQueue& = delete;

    [[nodiscard]] static auto Create(vk::raii::Device& Device)
        -> std::expected<VulkanTransferCompletionQueue, ErrorMessage> {
        VulkanTransferCompletionQueue Queue;
        auto Sema = VulkanTimelineSemaphore::Create(Device);
        if (!Sema)
            return std::unexpected(
                Sema.error().Append("VulkanTransferCompletionQueue: timeline semaphore creation failed"));
        Queue.m_Device   = &Device;
        Queue.m_Timeline = std::move(*Sema);
        return Queue;
    }

    /// Enqueue a callback to fire when transfer timeline reaches Token.
    auto EnqueueCallback(RHIGpuCompletionToken Token, std::function<void()> Fn) -> void {
        m_Queue.push_back(Entry{.Token = Token, .Fn = std::move(Fn)});
    }

    /// Allocate the next transfer completion token and matching signal info.
    [[nodiscard]] auto AllocateSignalSubmitInfo(vk::PipelineStageFlagBits2 Stage)
        -> std::pair<RHIGpuCompletionToken, vk::SemaphoreSubmitInfo> {
        RHIGpuCompletionToken Token{.Id = m_Timeline.NextValue()};
        return {Token,
                vk::SemaphoreSubmitInfo{
                    .semaphore = m_Timeline.Get(),
                    .value     = Token.Id,
                    .stageMask = Stage,
                }};
    }

    /// Non-blocking completion query for transfer upload tokens.
    [[nodiscard]] auto IsComplete(RHIGpuCompletionToken Token) -> bool {
        if (Token.Id == 0)
            return true;

        auto Current = m_Timeline.GetCurrentValue();
        if (!Current)
            return false;
        return *Current >= Token.Id;
    }

    /// Walk queue front-to-back, firing callbacks whose value is reached.
    /// Non-blocking — uses vkGetSemaphoreCounterValue.
    auto Tick() -> void {
        while (!m_Queue.empty()) {
            if (!IsComplete(m_Queue.front().Token))
                break;
            m_Queue.front().Fn();
            m_Queue.pop_front();
        }
    }

    /// Block until all enqueued items complete and fire remaining callbacks.
    /// Call during Shutdown.
    [[nodiscard]] auto Drain() -> std::expected<void, ErrorMessage> {
        if (m_Queue.empty())
            return {};
        auto LastValue = m_Queue.back().Token.Id;
        if (auto R = m_Timeline.Wait(LastValue); !R)
            return std::unexpected(R.error().Append("VulkanTransferCompletionQueue::Drain: wait failed"));
        Tick();
        return {};
    }

  private:
    struct Entry {
        RHIGpuCompletionToken   Token = {};
        std::function<void()> Fn    = {};
    };

    vk::raii::Device* m_Device = nullptr;
    VulkanTimelineSemaphore m_Timeline;
    std::deque<Entry> m_Queue;
};

} // namespace SoulEngine
