/// @file   ResourceHandle.cpp
/// @brief  Tests for ResourceContext-owned slots and handles.

#include <gtest/gtest.h>

import Resource;
import TaskGraph;
import std;

using namespace SoulEngine;

class ResourceManagerTest : public testing::Test {
  protected:
    auto SetUp() -> void override {
        ResourceManager::Get().BeginShutdown();
        TaskGraph::Get().Shutdown();
        ResourceManager::Get().Clear();
        TaskGraph::Get().Init(1);
        ResourceManager::Get().Init();
    }

    auto TearDown() -> void override {
        ResourceManager::Get().BeginShutdown();
        TaskGraph::Get().Shutdown();
        ResourceManager::Get().Clear();
    }
};

TEST_F(ResourceManagerTest, EquivalentMeshBlasRequestsDeduplicateBeforeDependenciesAreReady) {
    auto MeshRef = ResourceManager::Get().RequestMeshRef("NotYetLoadedMesh.obj");
    ASSERT_TRUE(MeshRef);
    auto FirstRef  = ResourceManager::Get().RequestBottomLevelAccelerationStructureRef(MeshRef);
    auto SecondRef = ResourceManager::Get().RequestBottomLevelAccelerationStructureRef(MeshRef);
    ASSERT_TRUE(FirstRef);
    ASSERT_TRUE(SecondRef);

    const auto& First  = FirstRef.GetHandle();
    const auto& Second = SecondRef.GetHandle();
    EXPECT_EQ(First.GetKey(), Second.GetKey());
    EXPECT_EQ(First.GetGeneration(), Second.GetGeneration());
    EXPECT_EQ(ResourceManager::Get().GetState(First), ResourceState::CpuPreparing);
}

TEST_F(ResourceManagerTest, RendererScopedTlasRequestsDeduplicateAndReleaseAsTransient) {
    const RHITopLevelAccelerationStructureDesc Desc{.InitialInstanceCapacity = 4};
    auto FirstRef  = ResourceManager::Get().RequestTopLevelAccelerationStructureRef("RayTracingRendererMain", Desc);
    auto SecondRef = ResourceManager::Get().RequestTopLevelAccelerationStructureRef("RayTracingRendererMain", Desc);
    auto OtherScopeRef =
        ResourceManager::Get().RequestTopLevelAccelerationStructureRef("RayTracingRendererReflection", Desc);
    ASSERT_TRUE(FirstRef);
    ASSERT_TRUE(SecondRef);
    ASSERT_TRUE(OtherScopeRef);

    const auto First = FirstRef.GetHandle();
    EXPECT_EQ(First.GetKey(), SecondRef.GetHandle().GetKey());
    EXPECT_EQ(First.GetGeneration(), SecondRef.GetHandle().GetGeneration());
    EXPECT_NE(First.GetKey(), OtherScopeRef.GetHandle().GetKey());

    FirstRef.Reset();
    EXPECT_EQ(ResourceManager::Get().GetState(First), ResourceState::CpuPreparing);
    SecondRef.Reset();
    EXPECT_EQ(ResourceManager::Get().GetState(First), ResourceState::Stale);
}

TEST_F(ResourceManagerTest, EmptyRendererScopePublishesFailure) {
    auto Ref = ResourceManager::Get().RequestTopLevelAccelerationStructureRef(
        "", RHITopLevelAccelerationStructureDesc{.InitialInstanceCapacity = 1});
    ASSERT_TRUE(Ref);
    EXPECT_EQ(ResourceManager::Get().GetState(Ref.GetHandle()), ResourceState::Failed);
}
