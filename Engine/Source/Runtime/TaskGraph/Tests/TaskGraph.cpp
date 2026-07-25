/// @file   TaskGraph.cpp
/// @brief  Tests for task dispatch queues and background workers.

#include <gtest/gtest.h>

import TaskGraph;
import std;

using namespace SoulEngine;

class TaskGraphTest : public testing::Test {
  protected:
    static auto SetUpTestSuite() -> void {
        TaskGraph::Get().Init(1);
    }

    static auto TearDownTestSuite() -> void {
        TaskGraph::Get().Shutdown();
    }
};

TEST_F(TaskGraphTest, ThreadQueueKeepsExistingEnqueueAndTryDequeueBehavior) {
    auto& Graph = TaskGraph::Get();
    bool      Ran = false;

    ASSERT_TRUE(Graph.Enqueue(ThreadQueue::Render, [&] { Ran = true; }).has_value());

    auto Task = Graph.TryDequeue(ThreadQueue::Render);
    ASSERT_TRUE(Task.has_value());
    (*Task)();

    EXPECT_TRUE(Ran);
    EXPECT_FALSE(Graph.TryDequeue(ThreadQueue::Render).has_value());
}

TEST_F(TaskGraphTest, BackgroundWorkerExecutesTasks) {
    auto&                   Graph = TaskGraph::Get();
    std::mutex              Mutex;
    std::condition_variable Cv;
    bool                    Ran = false;

    ASSERT_TRUE(Graph.EnqueueBackground([&] {
        {
            std::lock_guard Lock(Mutex);
            Ran = true;
        }
        Cv.notify_one();
    }).has_value());

    {
        std::unique_lock Lock(Mutex);
        EXPECT_TRUE(Cv.wait_for(Lock, std::chrono::seconds(2), [&] { return Ran; }));
    }

}

TEST_F(TaskGraphTest, ShutdownRejectsTasksAndAllowsRestart) {
    auto&             Graph = TaskGraph::Get();
    std::atomic<bool> Ran = false;

    Graph.Shutdown();
    auto Result = Graph.EnqueueBackground([&] { Ran.store(true, std::memory_order_release); });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    ASSERT_FALSE(Result.has_value());
    EXPECT_FALSE(Ran.load(std::memory_order_acquire));

    Graph.Init(1);
    ASSERT_TRUE(Graph.Enqueue(ThreadQueue::Game, [&] { Ran.store(true, std::memory_order_release); }).has_value());

    auto Task = Graph.TryDequeue(ThreadQueue::Game);
    ASSERT_TRUE(Task.has_value());
    (*Task)();

    EXPECT_TRUE(Ran.load(std::memory_order_acquire));
}
