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
    /// @brief Start background worker threads.
    /// @param WorkerCount Number of background worker threads to spawn.
    auto Init(std::size_t WorkerCount) -> void {
        Shutdown();

        {
            std::lock_guard Lock(m_BackgroundMutex);
            m_ShutdownRequested.store(false, std::memory_order_release);
        }

        m_Workers.reserve(WorkerCount);
        for (std::size_t WorkerIndex = 0; WorkerIndex < WorkerCount; ++WorkerIndex)
            m_Workers.emplace_back([this](std::stop_token Stop) { WorkerLoop(Stop); });

        LogInfo("Background worker threads spawned ({})", WorkerCount);
    }

    /// @brief Push a task onto the target thread's queue.
    /// Thread-safe. May be called from any thread.
    auto Enqueue(ThreadQueue Q, std::function<void()> Task) -> void {
        if (m_ShutdownRequested.load(std::memory_order_acquire))
            return;

        auto& [mtx, tasks] = m_Queues[static_cast<std::size_t>(Q)];
        std::lock_guard Lock(mtx);
        if (m_ShutdownRequested.load(std::memory_order_relaxed))
            return;

        tasks.push_back(std::move(Task));
    }

    /// @brief Push a task onto the background worker queue.
    /// Thread-safe. May be called from any thread.
    auto EnqueueBackground(std::function<void()> Task) -> void {
        {
            std::lock_guard Lock(m_BackgroundMutex);
            if (m_ShutdownRequested.load(std::memory_order_relaxed))
                return;
            m_BackgroundTasks.push_back(std::move(Task));
        }
        m_BackgroundCv.notify_one();
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
        m_ShutdownRequested.store(true, std::memory_order_release);
        m_BackgroundCv.notify_all();

        for (auto& Worker : m_Workers) {
            if (Worker.joinable()) {
                Worker.request_stop();
                Worker.join();
            }
        }
        m_Workers.clear();

        {
            std::lock_guard Lock(m_BackgroundMutex);
            m_BackgroundTasks.clear();
        }

        for (auto& [mtx, tasks] : m_Queues) {
            std::lock_guard Lock(mtx);
            tasks.clear();
        }
    }

    static constexpr std::size_t kMaxTasksPerPoll = 256;

  private:
    struct Queue {
        std::mutex                        Mutex;
        std::deque<std::function<void()>> Tasks;
    };

    auto WorkerLoop(std::stop_token Stop) -> void {
        while (!Stop.stop_requested()) {
            std::function<void()> Task;
            {
                std::unique_lock Lock(m_BackgroundMutex);
                m_BackgroundCv.wait(Lock, [&] {
                    return Stop.stop_requested() || m_ShutdownRequested.load(std::memory_order_relaxed) ||
                           !m_BackgroundTasks.empty();
                });

                if (Stop.stop_requested() || m_ShutdownRequested.load(std::memory_order_relaxed))
                    break;

                Task = std::move(m_BackgroundTasks.front());
                m_BackgroundTasks.pop_front();
            }

            Task();
        }
    }

    std::array<Queue, static_cast<std::size_t>(ThreadQueue::Count)> m_Queues = {};

    std::mutex                        m_BackgroundMutex;
    std::condition_variable           m_BackgroundCv;
    std::deque<std::function<void()>> m_BackgroundTasks;
    std::vector<std::jthread>         m_Workers;
    std::atomic<bool>                 m_ShutdownRequested = false;
};

} // namespace SoulEngine
