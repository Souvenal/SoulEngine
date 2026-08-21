#include <gtest/gtest.h>

#include <entt/entt.hpp>
#include <hlsl++.h>

import Core;

using namespace SoulEngine;

namespace {

class NoOpSystem final : public ISystem {
  public:
    explicit NoOpSystem(entt::registry& Registry) : ISystem(Registry) {}

    auto OnUpdate(Float32) -> void override {}
};

} // namespace

TEST(TransformTest, BuildsLocalMatrixFromTranslationRotationAndScale) {
    const Transform Transform{
        .Translation = hlslpp::float3(1.25f, -2.0f, 3.5f),
        .Rotation    = hlslpp::float3(0.0f, 0.0f, 0.0f),
        .Scale       = hlslpp::float3(2.0f, 3.0f, 4.0f),
    };

    const auto Matrix = Transform.GetLocalMatrix();
    EXPECT_FLOAT_EQ(static_cast<float>(Matrix[0].x), 2.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Matrix[1].y), 3.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Matrix[2].z), 4.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Matrix[3].x), 1.25f);
    EXPECT_FLOAT_EQ(static_cast<float>(Matrix[3].y), -2.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(Matrix[3].z), 3.5f);
}

TEST(SystemSchedulerTest, IgnoresDuplicateSystemRegistration) {
    entt::registry Registry;
    SystemScheduler Scheduler{Registry};
    Scheduler.Register<NoOpSystem>();
    const auto* First = Scheduler.Get<NoOpSystem>();
    ASSERT_NE(First, nullptr);

    Scheduler.Register<NoOpSystem>();

    EXPECT_EQ(Scheduler.Get<NoOpSystem>(), First);
}

TEST(TransformSystemTest, UpdatesDirtyTransformSubtrees) {
    entt::registry  Registry;
    const auto      HiddenRoot = Registry.create();
    Registry.emplace<TransformComponent>(HiddenRoot);

    TransformSystem System{Registry};
    System.SetupObservers();

    const auto Parent = Registry.create();
    const auto Child  = Registry.create();
    Registry.emplace<TransformComponent>(Parent);
    Registry.emplace<TransformComponent>(Child);
    Registry.emplace<ParentComponent>(Parent, HiddenRoot);
    Registry.emplace<ChildrenComponent>(Parent, ChildrenComponent{.Children = {Child}});
    Registry.emplace<ParentComponent>(Child, Parent);
    EXPECT_EQ(Registry.get<ParentComponent>(Parent).Depth, 1u);
    EXPECT_EQ(Registry.get<ParentComponent>(Child).Depth, 2u);

    Registry.patch<TransformComponent>(Child, [](auto& Transform) -> void {
        Transform.Translation = hlslpp::float3(0.0f, 3.0f, 0.0f);
    });
    Registry.patch<TransformComponent>(Parent, [](auto& Transform) -> void {
        Transform.Translation = hlslpp::float3(2.0f, 0.0f, 0.0f);
    });

    System.OnUpdate(0.0f);

    const auto& InitialChild = Registry.get<TransformComponent>(Child);
    EXPECT_FLOAT_EQ(static_cast<float>(InitialChild.WorldTransform[3].x), 2.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(InitialChild.WorldTransform[3].y), 3.0f);
    System.ClearObservers();

    Registry.patch<TransformComponent>(Parent, [](auto& Transform) -> void {
        Transform.Translation = hlslpp::float3(5.0f, 0.0f, 0.0f);
    });
    System.OnUpdate(0.0f);

    const auto& UpdatedChild = Registry.get<TransformComponent>(Child);
    EXPECT_FLOAT_EQ(static_cast<float>(UpdatedChild.WorldTransform[3].x), 5.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(UpdatedChild.WorldTransform[3].y), 3.0f);

    System.TeardownObservers();
}
