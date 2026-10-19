module;

#include <tracy/Tracy.hpp>

module TaskGraph:Runtime.impl;

import :Runtime;

namespace SoulEngine {

auto TaskGraph::Init(std::size_t WorkerCount) -> void {
    std::scoped_lock LifecycleLock(m_LifecycleMutex);
    if (m_Running.exchange(true, std::memory_order_acq_rel)) {
        LogWarning("TaskGraph initialization ignored; the process-global instance is already running");
        return;
    }

    m_Workers.reserve(WorkerCount);
    for (std::size_t WorkerIndex = 0; WorkerIndex < WorkerCount; ++WorkerIndex)
        m_Workers.emplace_back([this, WorkerIndex](std::stop_token Stop) { WorkerLoop(WorkerIndex, Stop); });

    LogInfo("Background worker threads spawned ({})", WorkerCount);
}

auto TaskGraph::EnqueueTask(ThreadQueue Q, std::function<void()> Task) -> std::expected<void, ErrorMessage> {
    if (!m_Running.load(std::memory_order_acquire))
        return std::unexpected(ErrorMessage("TaskGraph is not running"));

    auto&            Queue = m_Queues[std::to_underlying(Q)];
    std::scoped_lock Lock(Queue.Mutex);
    if (!m_Running.load(std::memory_order_relaxed))
        return std::unexpected(ErrorMessage("TaskGraph is not running"));

    Queue.Tasks.push(std::move(Task));
    return {};
}

auto TaskGraph::EnqueueFrameTask(ThreadQueue Q, std::function<void()> Task) -> std::expected<void, ErrorMessage> {
    const auto FrameIndex = GetThreadFrameIndex();
    if (FrameIndex == 0)
        return std::unexpected(ErrorMessage("No frame is active on the submitting thread"));
    return EnqueueFrameTask(Q, FrameIndex, std::move(Task));
}

auto TaskGraph::EnqueueFrameTask(ThreadQueue Q, Uint64 FrameIndex, std::function<void()> Task)
    -> std::expected<void, ErrorMessage> {
    if (!m_Running.load(std::memory_order_acquire))
        return std::unexpected(ErrorMessage("TaskGraph is not running"));

    auto&            Queue = m_Queues[std::to_underlying(Q)];
    std::scoped_lock Lock(Queue.Mutex);
    if (!m_Running.load(std::memory_order_relaxed))
        return std::unexpected(ErrorMessage("TaskGraph is not running"));

    // Sequence is assigned while holding the queue mutex, so concurrent producers receive a unique FIFO
    // order for tasks targeting the same frame.
    Queue.FrameTasks.push_back(FrameTask{
        .FrameIndex = FrameIndex,
        .Sequence   = Queue.NextFrameTaskSequence++,
        .Task       = std::move(Task),
    });
    std::push_heap(Queue.FrameTasks.begin(), Queue.FrameTasks.end(), std::greater<FrameTask>{});
    return {};
}

auto TaskGraph::IncreaseThreadFrameIndex() -> void {
    ++ThreadFrameIndex;
}

auto TaskGraph::GetThreadFrameIndex() const -> Uint64 {
    return ThreadFrameIndex;
}

auto TaskGraph::EnqueueBackground(std::function<void()> Task) -> std::expected<void, ErrorMessage> {
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

auto TaskGraph::DrainTasks(ThreadQueue Q) -> void {
    while (true) {
        std::queue<std::function<void()>> PendingTasks;
        {
            auto&            Queue = m_Queues[std::to_underlying(Q)];
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

auto TaskGraph::DrainFrameTasks(ThreadQueue Q) -> void {
    // The consumer uses its own TLS ordinal. It is intentionally independent from the producer's ordinal
    // captured by EnqueueFrameTask.
    const auto FrameIndex = GetThreadFrameIndex();
    while (true) {
        std::deque<FrameTask> PendingTasks;
        {
            auto&            Queue = m_Queues[std::to_underlying(Q)];
            std::scoped_lock Lock(Queue.Mutex);

            // FrameTasks is a min-heap. Only the heap front can be eligible: older tasks are stale, the
            // current frame is executable, and a larger frame must remain queued until the consumer
            // catches up.
            while (!Queue.FrameTasks.empty()) {
                if (Queue.FrameTasks.front().FrameIndex > FrameIndex)
                    break;

                // pop_heap moves the smallest (FrameIndex, Sequence) entry to the back, where it can be
                // moved out without copying its task.
                std::pop_heap(Queue.FrameTasks.begin(), Queue.FrameTasks.end(), std::greater<FrameTask>{});
                auto Task = std::move(Queue.FrameTasks.back());
                Queue.FrameTasks.pop_back();

                // A producer may have fallen behind the consumer. Such work missed its frame and is
                // discarded rather than executed late.
                if (Task.FrameIndex == FrameIndex)
                    PendingTasks.push_back(std::move(Task));
            }
        }

        if (PendingTasks.empty())
            return;

        // Never invoke user callbacks while holding Queue.Mutex. A task may enqueue more work, and newly
        // enqueued current-frame tasks are picked up by the next iteration of this drain.
        for (auto& Task : PendingTasks)
            Task.Task();
    }
}

auto TaskGraph::Shutdown() -> void {
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

TaskGraph::~TaskGraph() {
    Shutdown();
}

auto TaskGraph::WorkerLoop(std::size_t WorkerIndex, std::stop_token Stop) -> void {
    // SetThreadName copies the string, so a formatted local is fine here; pointer identity only matters
    // for names embedded in events
    const String ThreadName = Format("WorkerThread{}", WorkerIndex);
    tracy::SetThreadName(ThreadName.c_str());
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

} // namespace SoulEngine
