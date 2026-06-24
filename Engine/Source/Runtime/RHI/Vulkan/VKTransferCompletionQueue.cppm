module;

export module Vulkan:TransferCompletionQueue;

import Core;
import RHI;
import vulkan;
import std;

import :Semaphore;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

/// Transfer-completion queue backed by a dedicated timeline semaphore.
///
/// HostBuffer staging uploads, DeviceBuffer transfers, and other GPU resources
/// that must outlive their submitting queue operation are enqueued with a
/// timeline value. Tick() fires callbacks whose timeline value has been reached
/// (non-blocking, safe to call every frame).
///
/// Owns its own vk::SemaphoreType::eTimeline — separate from the frame-level
/// timeline in RenderDevice. This guarantees monotonic signal ordering on the
/// transfer queue without cross-queue ordering constraints.
class TransferCompletionQueue {
  public:
    TransferCompletionQueue() = default;

    TransferCompletionQueue(TransferCompletionQueue&&)                    = default;
    auto operator=(TransferCompletionQueue&&) -> TransferCompletionQueue& = default;

    TransferCompletionQueue(const TransferCompletionQueue&)                    = delete;
    auto operator=(const TransferCompletionQueue&) -> TransferCompletionQueue& = delete;

    [[nodiscard]] static auto Create(vk::raii::Device& Device)
        -> std::expected<TransferCompletionQueue, ErrorMessage> {
        TransferCompletionQueue Queue;
        auto Sema = TimelineSemaphore::Create(Device);
        if (!Sema)
            return std::unexpected(
                Sema.error().Append("TransferCompletionQueue: timeline semaphore creation failed"));
        Queue.m_Device   = &Device;
        Queue.m_Timeline = std::move(*Sema);
        return Queue;
    }

    /// Enqueue a callback to fire when transfer timeline reaches Token.
    auto EnqueueCallback(GpuCompletionToken Token, std::function<void()> Fn) -> void {
        m_Queue.push_back(Entry{.Token = Token, .Fn = std::move(Fn)});
    }

    /// Allocate the next transfer completion token and matching signal info.
    [[nodiscard]] auto AllocateSignalSubmitInfo(vk::PipelineStageFlagBits2 Stage)
        -> std::pair<GpuCompletionToken, vk::SemaphoreSubmitInfo> {
        GpuCompletionToken Token{.Id = m_Timeline.NextValue()};
        return {Token,
                vk::SemaphoreSubmitInfo{
                    .semaphore = m_Timeline.Get(),
                    .value     = Token.Id,
                    .stageMask = Stage,
                }};
    }

    /// Non-blocking completion query for transfer upload tokens.
    [[nodiscard]] auto IsComplete(GpuCompletionToken Token) -> bool {
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
            return std::unexpected(R.error().Append("TransferCompletionQueue::Drain: wait failed"));
        Tick();
        return {};
    }

  private:
    struct Entry {
        GpuCompletionToken   Token = {};
        std::function<void()> Fn    = {};
    };

    vk::raii::Device* m_Device = nullptr;
    TimelineSemaphore m_Timeline;
    std::deque<Entry> m_Queue;
};

} // namespace SoulEngine::RHI::Vulkan
