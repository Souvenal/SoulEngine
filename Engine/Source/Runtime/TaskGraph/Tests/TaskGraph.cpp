/// @file   TaskGraph.cpp
/// @brief  Tests for task dispatch queues and background workers.

#include <gtest/gtest.h>

import TaskGraph;
import std;

using namespace SoulEngine;

TEST(TaskGraphTest, ThreadQueueKeepsExistingEnqueueAndTryDequeueBehavior) {
    TaskGraph Graph;
    bool      Ran = false;

    Graph.Enqueue(ThreadQueue::Render, [&] { Ran = true; });

    auto Task = Graph.TryDequeue(ThreadQueue::Render);
    ASSERT_TRUE(Task.has_value());
    (*Task)();

    EXPECT_TRUE(Ran);
    EXPECT_FALSE(Graph.TryDequeue(ThreadQueue::Render).has_value());
}

TEST(TaskGraphTest, BackgroundWorkerExecutesTasks) {
    TaskGraph               Graph;
    std::mutex              Mutex;
    std::condition_variable Cv;
    bool                    Ran = false;

    Graph.Init(1);
    Graph.EnqueueBackground([&] {
        {
            std::lock_guard Lock(Mutex);
            Ran = true;
        }
        Cv.notify_one();
    });

    {
        std::unique_lock Lock(Mutex);
        EXPECT_TRUE(Cv.wait_for(Lock, std::chrono::seconds(2), [&] { return Ran; }));
    }

    Graph.Shutdown();
}

TEST(TaskGraphTest, ShutdownStopsAcceptingBackgroundTasks) {
    TaskGraph         Graph;
    std::atomic<bool> Ran = false;

    Graph.Init(1);
    Graph.Shutdown();
    Graph.EnqueueBackground([&] { Ran.store(true, std::memory_order_release); });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    EXPECT_FALSE(Ran.load(std::memory_order_acquire));
}
