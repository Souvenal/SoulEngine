module;

#include <tracy/Tracy.hpp>

export module TaskGraph;

import std;
import Core;
import magic_enum;

namespace SoulEngine {

/// @brief Task that may execute only when its published frame is current.
export struct FrameTask {
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
export enum class ThreadQueue {
    Game = 0,
    Render,
    RHI,
};

export struct TaskQueue {
    std::mutex                        Mutex;
    std::queue<std::function<void()>> Tasks;
    std::deque<FrameTask>             FrameTasks;
    Uint64                            NextFrameTaskSequence = 0;
};

/// @brief Thread-safe multi-queue task dispatcher.
///
/// 3 queues keyed by ThreadQueue. Each internal queue contains unbound tasks and frame-affined tasks. Drain
/// methods release the queue mutex before invoking user callbacks.
export class TaskGraph final : public Singleton<TaskGraph> {
    friend class Singleton<TaskGraph>;

  public:
    /// @brief Start background worker threads.
    /// @param WorkerCount Number of background worker threads to spawn.
    auto Init(std::size_t WorkerCount) -> void;

    /// @brief Push an unbound task onto the target thread's queue.
    /// Thread-safe. May be called from any thread.
    [[nodiscard]] auto EnqueueTask(ThreadQueue Q, std::function<void()> Task) -> std::expected<void, ErrorMessage>;

    /// @brief Push a task bound to the submitting thread's current frame.
    ///
    /// Game, Render, and RHI loops maintain independent thread-local frame counters. They do not read one
    /// another's TLS values; because each loop advances once per pipeline frame, the ordinal captured by the
    /// producer matches the ordinal consumed by the target thread when it catches up. Background workers must
    /// use the explicit-frame overload below.
    [[nodiscard]] auto EnqueueFrameTask(ThreadQueue Q, std::function<void()> Task) -> std::expected<void, ErrorMessage>;

    /// @brief Push a frame task with an explicitly supplied frame ordinal.
    /// Thread-safe. Intended for background workers and exceptional handoff paths.
    [[nodiscard]] auto EnqueueFrameTask(ThreadQueue Q, Uint64 FrameIndex, std::function<void()> Task)
        -> std::expected<void, ErrorMessage>;

    /// @brief Advance this thread's frame ordinal and return the new value.
    ///
    /// The three engine loops intentionally own independent TLS counters. This is an ordinal local to the
    /// loop, not a shared global frame ID.
    auto IncreaseThreadFrameIndex() -> void;

    /// @brief Read this thread's current frame ordinal.
    [[nodiscard]] auto GetThreadFrameIndex() const -> Uint64;

    /// @brief Push a task onto the background worker queue.
    /// Thread-safe. May be called from any thread.
    [[nodiscard]] auto EnqueueBackground(std::function<void()> Task) -> std::expected<void, ErrorMessage>;

    /// @brief Execute all unbound tasks currently pending on a thread queue.
    auto DrainTasks(ThreadQueue Q) -> void;

    /// @brief Execute frame tasks matching this thread's current frame ordinal.
    auto DrainFrameTasks(ThreadQueue Q) -> void;

    auto Shutdown() -> void;

  private:
    TaskGraph() = default;
    ~TaskGraph();
    auto WorkerLoop(std::size_t WorkerIndex, std::stop_token Stop) -> void;

    std::array<TaskQueue, magic_enum::enum_count<ThreadQueue>()> m_Queues = {};

    // Each fixed engine loop advances its own ordinal once per frame. TLS is intentionally not synchronized
    // across threads; Enqueue copies the producer's ordinal into FrameTask, while Drain reads the consumer's
    // ordinal. The pipeline's ordered frame progression makes matching ordinals refer to the same logical
    // frame when the consumer catches up.
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
