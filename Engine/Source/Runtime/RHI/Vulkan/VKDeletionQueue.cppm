module;

export module Vulkan:DeletionQueue;

import Core;
import RHI;
import :Semaphore;
import std;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

// ═════════════════════════════════════════════════════════════════════════════
// DeletionQueue — thread-safe deferred GPU resource retirement
// ═════════════════════════════════════════════════════════════════════════════

/// Manages deferred destruction of GPU resources.  Resources enqueue a
/// callback + a GPU completion token.  Tick() fires callbacks whose token
/// has passed on the timeline.  Drain() waits for the last pending token
/// then fires everything.
class DeletionQueue {
  public:
    struct Record {
        RHI::GpuCompletionToken RetireToken;
        std::function<void()>   Callback;
    };

    DeletionQueue() = default;

    explicit DeletionQueue(TimelineSemaphore& FrameTimeline) : m_FrameTimeline(&FrameTimeline) {}

    DeletionQueue(DeletionQueue&& Other) noexcept
        : m_FrameTimeline(std::exchange(Other.m_FrameTimeline, nullptr)), m_Records(std::move(Other.m_Records)) {}

    auto operator=(DeletionQueue&& Other) noexcept -> DeletionQueue& {
        if (this != &Other) {
            m_FrameTimeline = std::exchange(Other.m_FrameTimeline, nullptr);
            m_Records       = std::move(Other.m_Records);
        }
        return *this;
    }

    DeletionQueue(const DeletionQueue&)                    = delete;
    auto operator=(const DeletionQueue&) -> DeletionQueue& = delete;

    /// Thread-safe enqueue — may be called from any thread.
    auto Enqueue(RHI::GpuCompletionToken Token, std::function<void()> Callback) -> void {
        std::lock_guard Lock(m_Mutex);
        m_Records.emplace_back(Token, std::move(Callback));
    }

    /// Call only from the RHI thread (BeginFrame).  Retires records whose
    /// token is <= the current frame timeline value.
    auto Tick() -> void {
        if (!m_FrameTimeline) {
            LogError("DeletionQueue::Tick called with null m_FrameTimeline");
            return;
        }
        auto CurrentRes = m_FrameTimeline->GetCurrentValue();
        if (!CurrentRes) {
            LogError("DeletionQueue::Tick failed to query frame timeline: {}", CurrentRes.error().ToString());
            return;
        }
        const Uint64 Current = *CurrentRes;

        std::lock_guard Lock(m_Mutex);
        while (!m_Records.empty() && m_Records.front().RetireToken.Id <= Current) {
            m_Records.front().Callback();
            m_Records.pop_front();
        }
    }

    /// Drain all pending records — blocks CPU until the last token is
    /// signaled.  Call only from RHI thread (Shutdown).
    [[nodiscard]] auto Drain() -> std::expected<void, ErrorMessage> {
        if (!m_FrameTimeline) {
            return std::unexpected(ErrorMessage("DeletionQueue::Drain called with null m_FrameTimeline"));
        }
        std::lock_guard Lock(m_Mutex);
        if (!m_Records.empty()) {
            if (auto R = m_FrameTimeline->Wait(m_Records.back().RetireToken.Id); !R)
                return std::unexpected(R.error().Append("DeletionQueue::Drain failed"));
        }
        while (!m_Records.empty()) {
            m_Records.front().Callback();
            m_Records.pop_front();
        }
        return {};
    }

  private:
    TimelineSemaphore* m_FrameTimeline = nullptr;
    std::deque<Record> m_Records;
    std::mutex         m_Mutex;
};

} // namespace SoulEngine::RHI::Vulkan
