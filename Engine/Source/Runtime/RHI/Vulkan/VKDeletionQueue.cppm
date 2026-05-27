module;

export module Vulkan:DeletionQueue;

import Core;
import vulkan;
import std;

import :Semaphore;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

/// Deferred GPU resource deletion backed by a dedicated timeline semaphore.
///
/// HostBuffer staging uploads, DeviceBuffer transfers, and other GPU resources
/// that must outlive their submitting queue operation are enqueued with a
/// timeline value. Tick() fires callbacks whose timeline value has been reached
/// (non-blocking, safe to call every frame).
///
/// Owns its own vk::SemaphoreType::eTimeline — separate from the frame-level
/// timeline in RenderDevice. This guarantees monotonic signal ordering on the
/// transfer queue without cross-queue ordering constraints.
class DeferredDeletionQueue {
  public:
    DeferredDeletionQueue() = default;

    DeferredDeletionQueue(DeferredDeletionQueue&&)            = default;
    auto operator=(DeferredDeletionQueue&&) -> DeferredDeletionQueue& = default;

    DeferredDeletionQueue(const DeferredDeletionQueue&)            = delete;
    auto operator=(const DeferredDeletionQueue&) -> DeferredDeletionQueue& = delete;

    [[nodiscard]] static auto Create(vk::raii::Device& Device)
        -> std::expected<DeferredDeletionQueue, ErrorMessage> {
        DeferredDeletionQueue Queue;
        auto Sema = TimelineSemaphore::Create(Device);
        if (!Sema)
            return std::unexpected(Sema.error().Append("DeferredDeletionQueue: timeline semaphore creation failed"));
        Queue.m_Device   = &Device;
        Queue.m_Timeline = std::move(*Sema);
        return Queue;
    }

    /// Enqueue a destruction callback to fire when timeline reaches Value.
    auto Enqueue(Uint64 Value, std::function<void()> Fn) -> void {
        m_Queue.push_back(Entry{.Value = Value, .Fn = std::move(Fn)});
    }

    /// Returns SemaphoreSubmitInfo with next timeline value (calls NextValue).
    [[nodiscard]] auto GetSignalSubmitInfo(vk::PipelineStageFlagBits2 Stage = vk::PipelineStageFlagBits2::eNone) -> vk::SemaphoreSubmitInfo {
        return m_Timeline.GetSignalSubmitInfo(Stage);
    }

    /// Returns SemaphoreSubmitInfo for a specific value (no NextValue call).
    /// Stage must not be eNone for signal semaphores.
    [[nodiscard]] auto MakeSignalSubmitInfo(Uint64 Value, vk::PipelineStageFlagBits2 Stage) -> vk::SemaphoreSubmitInfo {
        return vk::SemaphoreSubmitInfo{
            .semaphore = m_Timeline.Get(),
            .value     = Value,
            .stageMask = Stage,
        };
    }

    /// Allocate next timeline value (for paired submit + enqueue).
    [[nodiscard]] auto NextValue() -> Uint64 {
        return m_Timeline.NextValue();
    }

    /// Returns reference to internal timeline semaphore.
    [[nodiscard]] auto GetTimeline() -> TimelineSemaphore& {
        return m_Timeline;
    }

    /// Walk queue front-to-back, firing callbacks whose value is reached.
    /// Non-blocking — uses vkGetSemaphoreCounterValue.
    auto Tick() -> void {
        while (!m_Queue.empty()) {
            auto Current = m_Timeline.GetCurrentValue();
            if (!Current)
                break;
            if (*Current < m_Queue.front().Value)
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
        auto LastValue = m_Queue.back().Value;
        if (auto R = m_Timeline.Wait(LastValue); !R)
            return std::unexpected(R.error().Append("DeferredDeletionQueue::Drain: wait failed"));
        Tick();
        return {};
    }

  private:
    struct Entry {
        Uint64               Value;
        std::function<void()> Fn;
    };

    vk::raii::Device*   m_Device   = nullptr;
    TimelineSemaphore   m_Timeline;
    std::deque<Entry>   m_Queue;
};

} // namespace SoulEngine::RHI::Vulkan
