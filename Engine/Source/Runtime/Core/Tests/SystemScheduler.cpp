/// @file   SystemScheduler.cpp
/// @brief  Tests for SystemScheduler name-based dependency DAG ordering.

#include <gtest/gtest.h>

#include <entt/entt.hpp>

import Core;
import std;

using namespace SoulEngine;

namespace {

/// @brief System that appends its compile-time Id to a shared log on update.
template <int Id>
class RecordingSystem final : public ISystem {
  public:
    explicit RecordingSystem(entt::registry& Registry, std::vector<int>& Log) : ISystem(Registry), m_Log(&Log) {}

    auto OnUpdate(Float32) -> void override {
        m_Log->push_back(Id);
    }

  private:
    std::vector<int>* m_Log = nullptr;
};

struct SchedulerFixture {
    entt::registry   Registry = {};
    SystemScheduler  Scheduler{Registry};
    std::vector<int> Log = {};
};

/// @brief Index of the first occurrence of Value in Log, or Log.size().
[[nodiscard]] auto PositionOf(const std::vector<int>& Log, int Value) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::find(Log, Value) - Log.begin());
}

} // namespace

TEST(SystemSchedulerTest, IndependentSystemsRunInRegistrationOrder) {
    SchedulerFixture F;
    ASSERT_TRUE(F.Scheduler.Register<RecordingSystem<0>>("SystemA", {}, {}, F.Log).has_value());
    ASSERT_TRUE(F.Scheduler.Register<RecordingSystem<1>>("SystemB", {}, {}, F.Log).has_value());
    ASSERT_TRUE(F.Scheduler.Register<RecordingSystem<2>>("SystemC", {}, {}, F.Log).has_value());
    ASSERT_TRUE(F.Scheduler.CompileDependency().has_value());

    ASSERT_TRUE(F.Scheduler.OnUpdate(0.0f).has_value());
    EXPECT_EQ(F.Log, (std::vector<int>{0, 1, 2}));
}

TEST(SystemSchedulerTest, BeforeConstraintIsRespected) {
    SchedulerFixture F;
    ASSERT_TRUE(F.Scheduler.Register<RecordingSystem<0>>("TransformSystem", {}, {}, F.Log).has_value());
    ASSERT_TRUE(F.Scheduler.Register<RecordingSystem<1>>("CameraSystem", {}, {}, F.Log).has_value());
    ASSERT_TRUE(
        F.Scheduler.Register<RecordingSystem<2>>("EditorCameraSystem", {"TransformSystem"}, {}, F.Log).has_value());
    ASSERT_TRUE(F.Scheduler.CompileDependency().has_value());

    ASSERT_TRUE(F.Scheduler.OnUpdate(0.0f).has_value());
    EXPECT_LT(PositionOf(F.Log, 2), PositionOf(F.Log, 0));
    // Ties break by registration order, so the full order is deterministic.
    EXPECT_EQ(F.Log, (std::vector<int>{1, 2, 0}));
}

TEST(SystemSchedulerTest, AfterConstraintIsSymmetricToBefore) {
    SchedulerFixture F;
    ASSERT_TRUE(F.Scheduler.Register<RecordingSystem<0>>("TransformSystem", {}, {"EditorCameraSystem"}, F.Log)
                    .has_value());
    ASSERT_TRUE(F.Scheduler.Register<RecordingSystem<1>>("EditorCameraSystem", {}, {}, F.Log).has_value());
    ASSERT_TRUE(F.Scheduler.CompileDependency().has_value());

    ASSERT_TRUE(F.Scheduler.OnUpdate(0.0f).has_value());
    EXPECT_LT(PositionOf(F.Log, 1), PositionOf(F.Log, 0));
}

TEST(SystemSchedulerTest, DuplicateEdgesAreHarmless) {
    SchedulerFixture F;
    ASSERT_TRUE(F.Scheduler.Register<RecordingSystem<0>>("SystemA", {"SystemB", "SystemB"}, {}, F.Log).has_value());
    ASSERT_TRUE(F.Scheduler.Register<RecordingSystem<1>>("SystemB", {}, {"SystemA"}, F.Log).has_value());
    ASSERT_TRUE(F.Scheduler.CompileDependency().has_value());

    ASSERT_TRUE(F.Scheduler.OnUpdate(0.0f).has_value());
    EXPECT_EQ(F.Log, (std::vector<int>{0, 1}));
}

TEST(SystemSchedulerTest, UnknownDependencyReferenceFails) {
    SchedulerFixture F;
    ASSERT_TRUE(F.Scheduler.Register<RecordingSystem<0>>("SystemA", {"TypoName"}, {}, F.Log).has_value());
    ASSERT_TRUE(F.Scheduler.Register<RecordingSystem<1>>("SystemB", {}, {"AnotherTypo"}, F.Log).has_value());

    const auto Result = F.Scheduler.CompileDependency();
    ASSERT_FALSE(Result.has_value());
    // Every dangling reference is reported in one pass.
    EXPECT_TRUE(Result.error().ToString().contains("TypoName"));
    EXPECT_TRUE(Result.error().ToString().contains("AnotherTypo"));
}

TEST(SystemSchedulerTest, DependencyCycleFailsWithConcretePath) {
    SchedulerFixture F;
    ASSERT_TRUE(F.Scheduler.Register<RecordingSystem<0>>("SystemA", {"SystemB"}, {}, F.Log).has_value());
    ASSERT_TRUE(F.Scheduler.Register<RecordingSystem<1>>("SystemB", {"SystemA"}, {}, F.Log).has_value());

    const auto Result = F.Scheduler.CompileDependency();
    ASSERT_FALSE(Result.has_value());
    EXPECT_TRUE(Result.error().ToString().contains("SystemA -> SystemB -> SystemA"));
}

TEST(SystemSchedulerTest, DuplicateNameFails) {
    SchedulerFixture F;
    ASSERT_TRUE(F.Scheduler.Register<RecordingSystem<0>>("SystemA", {}, {}, F.Log).has_value());

    const auto Result = F.Scheduler.Register<RecordingSystem<1>>("SystemA", {}, {}, F.Log);
    ASSERT_FALSE(Result.has_value());
    EXPECT_TRUE(Result.error().ToString().contains("Duplicate system name"));
}

TEST(SystemSchedulerTest, DuplicateTypeFails) {
    SchedulerFixture F;
    ASSERT_TRUE(F.Scheduler.Register<RecordingSystem<0>>("SystemA", {}, {}, F.Log).has_value());

    const auto Result = F.Scheduler.Register<RecordingSystem<0>>("SystemB", {}, {}, F.Log);
    ASSERT_FALSE(Result.has_value());
    EXPECT_TRUE(Result.error().ToString().contains("already registered"));
}

TEST(SystemSchedulerTest, RegisterAfterCompileFails) {
    SchedulerFixture F;
    ASSERT_TRUE(F.Scheduler.Register<RecordingSystem<0>>("SystemA", {}, {}, F.Log).has_value());
    ASSERT_TRUE(F.Scheduler.CompileDependency().has_value());

    const auto Result = F.Scheduler.Register<RecordingSystem<1>>("SystemB", {}, {}, F.Log);
    ASSERT_FALSE(Result.has_value());
    EXPECT_TRUE(Result.error().ToString().contains("only allowed before CompileDependency"));
}

TEST(SystemSchedulerTest, OnUpdateBeforeCompileFails) {
    SchedulerFixture F;
    ASSERT_TRUE(F.Scheduler.Register<RecordingSystem<0>>("SystemA", {}, {}, F.Log).has_value());

    const auto Result = F.Scheduler.OnUpdate(0.0f);
    ASSERT_FALSE(Result.has_value());
    EXPECT_TRUE(Result.error().ToString().contains("CompileDependency"));
}

TEST(SystemSchedulerTest, RemoveAfterCompileErasesFromExecutionOrder) {
    SchedulerFixture F;
    ASSERT_TRUE(F.Scheduler.Register<RecordingSystem<0>>("SystemA", {}, {}, F.Log).has_value());
    ASSERT_TRUE(F.Scheduler.Register<RecordingSystem<1>>("SystemB", {}, {}, F.Log).has_value());
    ASSERT_TRUE(F.Scheduler.Register<RecordingSystem<2>>("SystemC", {}, {}, F.Log).has_value());
    ASSERT_TRUE(F.Scheduler.CompileDependency().has_value());

    ASSERT_TRUE(F.Scheduler.Remove<RecordingSystem<1>>());
    EXPECT_EQ(F.Scheduler.Get<RecordingSystem<1>>(), nullptr);

    ASSERT_TRUE(F.Scheduler.OnUpdate(0.0f).has_value());
    EXPECT_EQ(F.Log, (std::vector<int>{0, 2}));
}
