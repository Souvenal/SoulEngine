module;

export module Vulkan:DeletionQueue;

import Core;
import RHI;
import :Semaphore;
import std;

namespace SoulEngine {

// ═════════════════════════════════════════════════════════════════════════════
// VulkanDeletionQueue — thread-safe deferred GPU resource retirement
// ═════════════════════════════════════════════════════════════════════════════

/// Manages deferred destruction of GPU resources.  Resources enqueue a
/// callback + a GPU completion token.  Tick() fires callbacks whose token
/// has passed on the timeline.  Drain() waits for the last pending token
/// then fires everything.
class VulkanDeletionQueue {
  public:
    struct Record {
        RHIFrameSubmissionToken RetireToken;
        std::function<void()>   Callback;
    };

    VulkanDeletionQueue() = default;

    explicit VulkanDeletionQueue(VulkanTimelineSemaphore& FrameTimeline) : m_FrameTimeline(&FrameTimeline) {}

    VulkanDeletionQueue(VulkanDeletionQueue&& Other) noexcept
        : m_FrameTimeline(std::exchange(Other.m_FrameTimeline, nullptr)), m_Records(std::move(Other.m_Records)) {}

    auto operator=(VulkanDeletionQueue&& Other) noexcept -> VulkanDeletionQueue& {
        if (this != &Other) {
            m_FrameTimeline = std::exchange(Other.m_FrameTimeline, nullptr);
            m_Records       = std::move(Other.m_Records);
        }
        return *this;
    }

    VulkanDeletionQueue(const VulkanDeletionQueue&)                    = delete;
    auto operator=(const VulkanDeletionQueue&) -> VulkanDeletionQueue& = delete;

    /// Thread-safe enqueue — may be called from any thread.
    auto Enqueue(RHIFrameSubmissionToken Token, std::function<void()> Callback) -> void {
        std::lock_guard Lock(m_Mutex);
        m_Records.emplace_back(Token, std::move(Callback));
    }

    /// Call only from the RHI thread (BeginFrame).  Retires records whose
    /// token is <= the current frame timeline value.
    auto Tick() -> void {
        if (!m_FrameTimeline) {
            LogError("VulkanDeletionQueue::Tick called with null m_FrameTimeline");
            return;
        }
        auto CurrentRes = m_FrameTimeline->GetCurrentValue();
        if (!CurrentRes) {
            LogError("VulkanDeletionQueue::Tick failed to query frame timeline: {}", CurrentRes.error().ToString());
            return;
        }
        const Uint64 Current = *CurrentRes;

        std::lock_guard Lock(m_Mutex);
        while (!m_Records.empty() && m_Records.front().RetireToken.Id <= Current) {
            m_Records.front().Callback();
            m_Records.pop_front();
        }
    }

    /// Drain all pending records. When @p bForce is false, blocks CPU until
    /// the last token is signaled. When true, caller must have completed
    /// RHIRenderDevice::WaitIdle() before calling.
    [[nodiscard]] auto Drain(bool bForce = false) -> std::expected<void, ErrorMessage> {
        if (!m_FrameTimeline) {
            return std::unexpected(ErrorMessage("VulkanDeletionQueue::Drain called with null m_FrameTimeline"));
        }
        std::lock_guard Lock(m_Mutex);
        if (!bForce && !m_Records.empty()) {
            if (auto R = m_FrameTimeline->Wait(m_Records.back().RetireToken.Id); !R)
                return std::unexpected(R.error().Append("VulkanDeletionQueue::Drain failed"));
        }
        while (!m_Records.empty()) {
            m_Records.front().Callback();
            m_Records.pop_front();
        }
        return {};
    }

  private:
    VulkanTimelineSemaphore* m_FrameTimeline = nullptr;
    std::deque<Record> m_Records;
    std::mutex         m_Mutex;
};

} // namespace SoulEngine
