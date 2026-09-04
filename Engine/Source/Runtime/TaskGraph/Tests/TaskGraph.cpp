/// @file   TaskGraph.cpp
/// @brief  Tests for task dispatch queues and background workers.

#include <gtest/gtest.h>

import std;
import TaskGraph;
import Core;

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

TEST_F(TaskGraphTest, DrainTasksExecutesAllUnboundTasks) {
    auto& Graph = TaskGraph::Get();
    std::vector<int> Executed = {};

    ASSERT_TRUE(Graph.EnqueueTask(ThreadQueue::Render, [&] { Executed.push_back(1); }).has_value());
    ASSERT_TRUE(Graph.EnqueueTask(ThreadQueue::Render, [&] { Executed.push_back(2); }).has_value());

    Graph.DrainTasks(ThreadQueue::Render);

    EXPECT_EQ(Executed, (std::vector<int>{1, 2}));
}

TEST_F(TaskGraphTest, DrainTasksAllowsTasksToEnqueueMoreTasks) {
    auto& Graph = TaskGraph::Get();
    bool      FollowUpRan = false;

    ASSERT_TRUE(Graph.EnqueueTask(ThreadQueue::Render, [&] {
        ASSERT_TRUE(Graph.EnqueueTask(ThreadQueue::Render, [&] { FollowUpRan = true; }).has_value());
    }).has_value());

    Graph.DrainTasks(ThreadQueue::Render);
    EXPECT_TRUE(FollowUpRan);
}

TEST_F(TaskGraphTest, FrameBoundTasksDoNotRunBeforeTheirFrame) {
    auto& Graph = TaskGraph::Get();
    Graph.IncreaseThreadFrameIndex();
    const auto          CurrentFrame = Graph.GetThreadFrameIndex();
    std::vector<Uint64> ExecutedFrames = {};

    ASSERT_TRUE(Graph.EnqueueFrameTask(ThreadQueue::RHI, CurrentFrame + 1,
                                       [&] { ExecutedFrames.push_back(CurrentFrame + 1); })
                    .has_value());
    ASSERT_TRUE(Graph.EnqueueFrameTask(ThreadQueue::RHI, [&] { ExecutedFrames.push_back(CurrentFrame); }).has_value());

    Graph.DrainFrameTasks(ThreadQueue::RHI);

    EXPECT_EQ(ExecutedFrames, std::vector<Uint64>{CurrentFrame});

    Graph.DrainFrameTasks(ThreadQueue::RHI);
    EXPECT_EQ(ExecutedFrames, std::vector<Uint64>{CurrentFrame});

    Graph.IncreaseThreadFrameIndex();
    EXPECT_EQ(Graph.GetThreadFrameIndex(), CurrentFrame + 1);
    Graph.DrainFrameTasks(ThreadQueue::RHI);
    EXPECT_EQ(ExecutedFrames, (std::vector<Uint64>{CurrentFrame, CurrentFrame + 1}));
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
    ASSERT_TRUE(Graph.EnqueueTask(ThreadQueue::Game, [&] { Ran.store(true, std::memory_order_release); }).has_value());

    Graph.DrainTasks(ThreadQueue::Game);

    EXPECT_TRUE(Ran.load(std::memory_order_acquire));
}
