module;

export module TaskGraph;

import std;
import Core;
import magic_enum;

export namespace SoulEngine {

/// @brief Task that may execute only when its published frame is current.
struct FrameTask {
    Uint64                FrameIndex = 0;
    Uint64                Sequence   = 0;
    std::function<void()> Task       = {};

    /// @brief Orders the min-heap by frame first, then preserves FIFO within a frame.
    [[nodiscard]] friend auto operator>(const FrameTask& Left, const FrameTask& Right) noexcept -> bool {
        if (Left.FrameIndex != Right.FrameIndex)
            return Left.FrameIndex > Right.FrameIndex;
        return Left.Sequence > Right.Sequence;
    }
};

/// @brief Thread queue target for task dispatch.
enum class ThreadQueue {
    Game = 0,
    Render,
    RHI,
};

/// @brief Thread-safe pair of unbound and frame-affined task queues.
struct TaskQueue {
    std::mutex                       Mutex;
    std::queue<std::function<void()>> Tasks;
    std::deque<FrameTask>             FrameTasks;
    Uint64                            NextFrameTaskSequence = 0;
};

/// @brief Thread-safe multi-queue task dispatcher.
///
/// 3 queues keyed by ThreadQueue. Each internal queue contains unbound tasks
/// and frame-affined tasks. Drain methods release the queue mutex before
/// invoking user callbacks.
class TaskGraph final : public Singleton<TaskGraph> {
    friend class Singleton<TaskGraph>;

  public:
    /// @brief Start background worker threads.
    /// @param WorkerCount Number of background worker threads to spawn.
    auto Init(std::size_t WorkerCount) -> void {
        std::scoped_lock LifecycleLock(m_LifecycleMutex);
        if (m_Running.exchange(true, std::memory_order_acq_rel)) {
            LogWarning("TaskGraph initialization ignored; the process-global instance is already running");
            return;
        }

        m_Workers.reserve(WorkerCount);
        for (std::size_t WorkerIndex = 0; WorkerIndex < WorkerCount; ++WorkerIndex)
            m_Workers.emplace_back([this](std::stop_token Stop) { WorkerLoop(Stop); });

        LogInfo("Background worker threads spawned ({})", WorkerCount);
    }

    /// @brief Push an unbound task onto the target thread's queue.
    /// Thread-safe. May be called from any thread.
    [[nodiscard]] auto EnqueueTask(ThreadQueue Q, std::function<void()> Task)
        -> std::expected<void, ErrorMessage> {
        if (!m_Running.load(std::memory_order_acquire))
            return std::unexpected(ErrorMessage("TaskGraph is not running"));

        auto& Queue = m_Queues[std::to_underlying(Q)];
        std::scoped_lock Lock(Queue.Mutex);
        if (!m_Running.load(std::memory_order_relaxed))
            return std::unexpected(ErrorMessage("TaskGraph is not running"));

        Queue.Tasks.push(std::move(Task));
        return {};
    }

    /// @brief Push a task bound to the submitting thread's current frame.
    ///
    /// Game, Render, and RHI loops maintain independent thread-local frame
    /// counters. They do not read one another's TLS values; because each loop
    /// advances once per pipeline frame, the ordinal captured by the producer
    /// matches the ordinal consumed by the target thread when it catches up.
    /// Background workers must use the explicit-frame overload below.
    [[nodiscard]] auto EnqueueFrameTask(ThreadQueue Q, std::function<void()> Task)
        -> std::expected<void, ErrorMessage> {
        const auto FrameIndex = GetThreadFrameIndex();
        if (FrameIndex == 0)
            return std::unexpected(ErrorMessage("No frame is active on the submitting thread"));
        return EnqueueFrameTask(Q, FrameIndex, std::move(Task));
    }

    /// @brief Push a frame task with an explicitly supplied frame ordinal.
    /// Thread-safe. Intended for background workers and exceptional handoff paths.
    [[nodiscard]] auto EnqueueFrameTask(ThreadQueue Q, Uint64 FrameIndex, std::function<void()> Task)
        -> std::expected<void, ErrorMessage> {
        if (!m_Running.load(std::memory_order_acquire))
            return std::unexpected(ErrorMessage("TaskGraph is not running"));

        auto& Queue = m_Queues[std::to_underlying(Q)];
        std::scoped_lock Lock(Queue.Mutex);
        if (!m_Running.load(std::memory_order_relaxed))
            return std::unexpected(ErrorMessage("TaskGraph is not running"));

        // Sequence is assigned while holding the queue mutex, so concurrent producers
        // receive a unique FIFO order for tasks targeting the same frame.
        Queue.FrameTasks.push_back(FrameTask{
            .FrameIndex = FrameIndex,
            .Sequence   = Queue.NextFrameTaskSequence++,
            .Task       = std::move(Task),
        });
        std::push_heap(Queue.FrameTasks.begin(), Queue.FrameTasks.end(), std::greater<FrameTask>{});
        return {};
    }

    /// @brief Advance this thread's frame ordinal and return the new value.
    ///
    /// The three engine loops intentionally own independent TLS counters.
    /// This is an ordinal local to the loop, not a shared global frame ID.
    auto IncreaseThreadFrameIndex() -> void {
        ++ThreadFrameIndex;
    }

    /// @brief Read this thread's current frame ordinal.
    [[nodiscard]] auto GetThreadFrameIndex() const -> Uint64 {
        return ThreadFrameIndex;
    }

    /// @brief Push a task onto the background worker queue.
    /// Thread-safe. May be called from any thread.
    [[nodiscard]] auto EnqueueBackground(std::function<void()> Task) -> std::expected<void, ErrorMessage> {
        if (!m_Running.load(std::memory_order_acquire))
            return std::unexpected(ErrorMessage("TaskGraph is not running"));

        {
            std::scoped_lock Lock(m_BackgroundMutex);
            if (!m_Running.load(std::memory_order_relaxed))
                return std::unexpected(ErrorMessage("TaskGraph is not running"));
            m_BackgroundTasks.push_back(std::move(Task));
        }
        m_BackgroundCv.notify_one();
        return {};
    }

    /// @brief Execute all unbound tasks currently pending on a thread queue.
    auto DrainTasks(ThreadQueue Q) -> void {
        while (true) {
            std::queue<std::function<void()>> PendingTasks;
            {
                auto& Queue = m_Queues[std::to_underlying(Q)];
                std::scoped_lock Lock(Queue.Mutex);
                PendingTasks.swap(Queue.Tasks);
            }

            if (PendingTasks.empty())
                return;

            while (!PendingTasks.empty()) {
                auto Task = std::move(PendingTasks.front());
                PendingTasks.pop();
                Task();
            }
        }
    }

    /// @brief Execute frame tasks matching this thread's current frame ordinal.
    auto DrainFrameTasks(ThreadQueue Q) -> void {
        // The consumer uses its own TLS ordinal. It is intentionally independent
        // from the producer's ordinal captured by EnqueueFrameTask.
        const auto FrameIndex = GetThreadFrameIndex();
        while (true) {
            std::deque<FrameTask> PendingTasks;
            {
                auto& Queue = m_Queues[std::to_underlying(Q)];
                std::scoped_lock Lock(Queue.Mutex);

                // FrameTasks is a min-heap. Only the heap front can be eligible:
                // older tasks are stale, the current frame is executable, and a
                // larger frame must remain queued until the consumer catches up.
                while (!Queue.FrameTasks.empty()) {
                    if (Queue.FrameTasks.front().FrameIndex > FrameIndex)
                        break;

                    // pop_heap moves the smallest (FrameIndex, Sequence) entry to
                    // the back, where it can be moved out without copying its task.
                    std::pop_heap(Queue.FrameTasks.begin(),
                                  Queue.FrameTasks.end(),
                                  std::greater<FrameTask>{});
                    auto Task = std::move(Queue.FrameTasks.back());
                    Queue.FrameTasks.pop_back();

                    // A producer may have fallen behind the consumer. Such work
                    // missed its frame and is discarded rather than executed late.
                    if (Task.FrameIndex == FrameIndex)
                        PendingTasks.push_back(std::move(Task));
                }
            }

            if (PendingTasks.empty())
                return;

            // Never invoke user callbacks while holding Queue.Mutex. A task may
            // enqueue more work, and newly enqueued current-frame tasks are picked
            // up by the next iteration of this drain.
            for (auto& Task : PendingTasks)
                Task.Task();
        }
    }

    auto Shutdown() -> void {
        std::scoped_lock LifecycleLock(m_LifecycleMutex);
        if (!m_Running.exchange(false, std::memory_order_acq_rel))
            return;

        for (auto& Worker : m_Workers) {
            if (Worker.joinable())
                Worker.request_stop();
        }

        m_BackgroundCv.notify_all();

        for (auto& Worker : m_Workers) {
            if (Worker.joinable())
                Worker.join();
        }
        m_Workers.clear();

        {
            std::scoped_lock Lock(m_BackgroundMutex);
            m_BackgroundTasks.clear();
        }

        for (auto& Queue : m_Queues) {
            std::scoped_lock Lock(Queue.Mutex);
            Queue.Tasks = {};
            Queue.FrameTasks.clear();
            Queue.NextFrameTaskSequence = 0;
        }
    }

  private:
    TaskGraph() = default;

    ~TaskGraph() {
        Shutdown();
    }

    auto WorkerLoop(std::stop_token Stop) -> void {
        SetLogThreadRole(LogThreadRole::Worker);
        while (!Stop.stop_requested()) {
            std::function<void()> Task;
            {
                std::unique_lock Lock(m_BackgroundMutex);
                m_BackgroundCv.wait(Lock, [&] {
                    return Stop.stop_requested() || !m_Running.load(std::memory_order_relaxed) ||
                           !m_BackgroundTasks.empty();
                });

                if (Stop.stop_requested() || !m_Running.load(std::memory_order_relaxed))
                    break;

                Task = std::move(m_BackgroundTasks.front());
                m_BackgroundTasks.pop_front();
            }

            Task();
        }
    }

    std::array<TaskQueue, magic_enum::enum_count<ThreadQueue>()> m_Queues = {};

    // Each fixed engine loop advances its own ordinal once per frame. TLS is
    // intentionally not synchronized across threads; Enqueue copies the
    // producer's ordinal into FrameTask, while Drain reads the consumer's
    // ordinal. The pipeline's ordered frame progression makes matching
    // ordinals refer to the same logical frame when the consumer catches up.
    inline static thread_local Uint64 ThreadFrameIndex = 0;

    // Serializes complete Init/Shutdown cycles so a restart cannot overlap teardown.
    std::mutex                        m_LifecycleMutex;
    std::mutex                        m_BackgroundMutex;
    std::condition_variable           m_BackgroundCv;
    std::deque<std::function<void()>> m_BackgroundTasks;
    std::vector<std::jthread>         m_Workers;
    // Gates task admission and tells waiting workers to exit during shutdown.
    std::atomic<bool>                 m_Running = false;
};

} // namespace SoulEngine
