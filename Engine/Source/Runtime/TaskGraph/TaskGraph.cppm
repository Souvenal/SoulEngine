export module TaskGraph;

import Core;
export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine {

/// @brief Thread queue target for Enqueue / TryDequeue.
enum class ThreadQueue {
    Game = 0,
    Render,
    RHI,
    Count,
};

/// @brief Thread-safe multi-queue task dispatcher.
///
/// 3 queues keyed by ThreadQueue. Each internal queue is mutex+deque.
/// Consumer calls TryDequeue(queue) per frame, capped at kMaxTasksPerPoll.
class TaskGraph {
  public:
    /// @brief Push a task onto the target thread's queue.
    /// Thread-safe. May be called from any thread.
    auto Enqueue(ThreadQueue Q, std::function<void()> Task) -> void {
        auto& [mtx, tasks] = m_Queues[static_cast<std::size_t>(Q)];
        std::lock_guard Lock(mtx);
        tasks.push_back(std::move(Task));
    }

    /// @brief Non-blocking dequeue from a thread's queue.
    /// Returns std::nullopt when empty.
    [[nodiscard]] auto TryDequeue(ThreadQueue Q) -> std::optional<std::function<void()>> {
        auto& [mtx, tasks] = m_Queues[static_cast<std::size_t>(Q)];
        std::lock_guard Lock(mtx);
        if (tasks.empty())
            return std::nullopt;
        auto Task = std::move(tasks.front());
        tasks.pop_front();
        return Task;
    }

    auto Shutdown() -> void {
        for (auto& [mtx, tasks] : m_Queues)
            tasks.clear();
    }

    static constexpr std::size_t kMaxTasksPerPoll = 256;

  private:
    struct Queue {
        std::mutex                        Mutex;
        std::deque<std::function<void()>> Tasks;
    };

    std::array<Queue, static_cast<std::size_t>(ThreadQueue::Count)> m_Queues = {};
};

} // namespace SoulEngine
