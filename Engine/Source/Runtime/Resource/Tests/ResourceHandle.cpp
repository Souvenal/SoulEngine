/// @file   ResourceHandle.cpp
/// @brief  Tests for asynchronous resource handle publication.

#include <gtest/gtest.h>

import Resource;
import TaskGraph;
import std;

using namespace SoulEngine::Core;
using namespace SoulEngine;
using namespace SoulEngine::Resource;

[[nodiscard]] auto IsPending(ResourceState State) -> bool {
    return State == ResourceState::CpuPreparing || State == ResourceState::RhiCommitting ||
           State == ResourceState::GpuPending;
}

TEST(ResourceHandleTest, ReadyPublishMakesHandleReady) {
    auto Resource = CreatePendingResource<SampledTextureResource>(42);

    EXPECT_EQ(Resource.Handle.GetState(), ResourceState::CpuPreparing);
    EXPECT_TRUE(Resource.Slot->PublishReady(Resource.Handle.GetGeneration(), SampledTextureResource{}));

    EXPECT_EQ(Resource.Handle.GetState(), ResourceState::Ready);
    EXPECT_TRUE(Resource.Handle.TryGet().has_value());
}

TEST(ResourceHandleTest, FailedPublishMakesHandleFailed) {
    auto Resource = CreatePendingResource<SampledTextureResource>(43);

    EXPECT_TRUE(Resource.Slot->PublishFailed(Resource.Handle.GetGeneration(), ErrorMessage{"missing texture"}));

    EXPECT_EQ(Resource.Handle.GetState(), ResourceState::Failed);
    ASSERT_TRUE(Resource.Handle.GetError().has_value());
    EXPECT_TRUE(Resource.Handle.GetError()->ToString().starts_with("missing texture"));
}

TEST(ResourceHandleTest, StaleGenerationPublishIgnored) {
    auto Resource         = CreatePendingResource<SampledTextureResource>(44);
    auto FirstGeneration  = Resource.Handle.GetGeneration();
    auto SecondGeneration = Resource.Slot->Reset(Resource.Handle.GetId());
    auto CurrentHandle =
        ResourceHandle<SampledTextureResource>::Create(Resource.Slot, Resource.Handle.GetId(), SecondGeneration);

    EXPECT_FALSE(Resource.Slot->PublishReady(FirstGeneration, SampledTextureResource{}));
    EXPECT_EQ(Resource.Handle.GetState(), ResourceState::Stale);
    EXPECT_EQ(CurrentHandle.GetState(), ResourceState::CpuPreparing);

    EXPECT_TRUE(Resource.Slot->PublishReady(SecondGeneration, SampledTextureResource{}));
    EXPECT_EQ(CurrentHandle.GetState(), ResourceState::Ready);
}

TEST(ResourceHandleTest, DefaultHandleStateIsUnknown) {
    SampledTextureHandle Handle;

    EXPECT_FALSE(Handle.IsValid());
    EXPECT_EQ(Handle.GetState(), ResourceState::Unknown);
    EXPECT_FALSE(Handle.TryGet().has_value());
}

TEST(ResourceSlotTest, ExplicitStateTransitionsPublishExpectedStates) {
    auto Resource = CreatePendingResource<SampledTextureResource>(46);

    EXPECT_EQ(Resource.Handle.GetState(), ResourceState::CpuPreparing);
    EXPECT_TRUE(Resource.Slot->MarkRhiCommitting(Resource.Handle.GetGeneration()));
    EXPECT_EQ(Resource.Handle.GetState(), ResourceState::RhiCommitting);
    EXPECT_TRUE(Resource.Slot->PublishGpuPending(Resource.Handle.GetGeneration()));
    EXPECT_EQ(Resource.Handle.GetState(), ResourceState::GpuPending);
    EXPECT_TRUE(Resource.Slot->PublishReady(Resource.Handle.GetGeneration(), SampledTextureResource{}));
    EXPECT_EQ(Resource.Handle.GetState(), ResourceState::Ready);
}

TEST(ResourceSlotTest, StaleGenerationCannotPublishPendingReadyOrFailed) {
    auto Resource         = CreatePendingResource<SampledTextureResource>(47);
    auto FirstGeneration  = Resource.Handle.GetGeneration();
    auto SecondGeneration = Resource.Slot->Reset(Resource.Handle.GetId());
    auto CurrentHandle =
        ResourceHandle<SampledTextureResource>::Create(Resource.Slot, Resource.Handle.GetId(), SecondGeneration);

    EXPECT_FALSE(Resource.Slot->MarkRhiCommitting(FirstGeneration));
    EXPECT_FALSE(Resource.Slot->PublishGpuPending(FirstGeneration));
    EXPECT_FALSE(Resource.Slot->PublishReady(FirstGeneration, SampledTextureResource{}));
    EXPECT_FALSE(Resource.Slot->PublishFailed(FirstGeneration, ErrorMessage{"old failure"}));
    EXPECT_EQ(Resource.Handle.GetState(), ResourceState::Stale);
    EXPECT_EQ(CurrentHandle.GetState(), ResourceState::CpuPreparing);
}

TEST(ResourceSampledTextureRequestTest, CoalescesNormalizedTexturePaths) {
    TaskGraph Graph;
    Manager::Get().Clear();
    Manager::Get().Init(Graph);

    auto A = Manager::Get().RequestSampledTexture("Assets/../Textures/Missing.png");
    auto B = Manager::Get().RequestSampledTexture("Textures/Missing.png");

    EXPECT_TRUE(A.IsValid());
    EXPECT_EQ(A.GetId(), B.GetId());
    EXPECT_EQ(A.GetGeneration(), B.GetGeneration());

    Graph.Shutdown();
    Manager::Get().Clear();
}

TEST(ResourceSampledTextureRequestTest, RejectsRequestsAfterShutdownBegins) {
    TaskGraph Graph;
    Manager::Get().Clear();
    Manager::Get().Init(Graph);
    Manager::Get().BeginShutdown();

    auto Handle = Manager::Get().RequestSampledTexture("Textures/Missing.png");

    EXPECT_FALSE(Handle.IsValid());

    Graph.Shutdown();
    Manager::Get().Clear();
    Manager::Get().Init(Graph);
}

TEST(ResourceSampledTextureRequestTest, MissingFilePublishesFailedResource) {
    TaskGraph Graph;
    Graph.Init(1);
    Manager::Get().Clear();
    Manager::Get().Init(Graph);

    auto Handle = Manager::Get().RequestSampledTexture("DefinitelyMissingTexture.png");

    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (IsPending(Handle.GetState()) && std::chrono::steady_clock::now() < Deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    EXPECT_EQ(Handle.GetState(), ResourceState::Failed);
    ASSERT_TRUE(Handle.GetError().has_value());
    EXPECT_TRUE(Handle.GetError()->ToString().starts_with("stbi_load failed"));

    Graph.Shutdown();
    Manager::Get().Clear();
}

TEST(ResourceSampledTextureRequestTest, StaleTexturePublishIgnored) {
    auto Resource        = CreatePendingResource<SampledTextureResource>(45);
    auto FirstGeneration = Resource.Handle.GetGeneration();
    auto NextGeneration  = Resource.Slot->Reset(Resource.Handle.GetId());

    EXPECT_FALSE(Resource.Slot->PublishFailed(FirstGeneration, ErrorMessage{"old failure"}));
    EXPECT_EQ(Resource.Handle.GetState(), ResourceState::Stale);
    EXPECT_EQ(Resource.Slot->GetState(NextGeneration), ResourceState::CpuPreparing);
}

TEST(ResourcePipelineRequestTest, CoalescesPipelineKeys) {
    TaskGraph Graph;
    Manager::Get().Clear();
    Manager::Get().Init(Graph);

    GraphicsPipelineRequest Req{
        .VertEntry = {.SourcePath = Path("Shaders/Test.slang"), .EntryPoint = "vertMain"},
        .FragEntry = {.SourcePath = Path("Shaders/Test.slang"), .EntryPoint = "fragMain"},
        .VertexInputLayout =
            RHI::VertexInputLayoutDesc{
                .Binding = 0,
                .Stride  = 32,
                .Attributes =
                    {
                        RHI::VertexInputAttributeDesc{
                            .Location = 0,
                            .Format   = RHI::Format::R32G32B32_SFLOAT,
                            .Offset   = 0,
                        },
                    },
            },
    };

    auto A = Manager::Get().RequestGraphicsPipeline(Req);
    auto B = Manager::Get().RequestGraphicsPipeline(Req);

    EXPECT_TRUE(A.IsValid());
    EXPECT_EQ(A.GetId(), B.GetId());
    EXPECT_EQ(A.GetGeneration(), B.GetGeneration());

    Graph.Shutdown();
    Manager::Get().Clear();
}

TEST(ResourcePipelineRequestTest, ShaderCompileFailurePublishesFailedResource) {
    TaskGraph Graph;
    Graph.Init(1);
    Manager::Get().Clear();
    Manager::Get().Init(Graph);

    auto Handle = Manager::Get().RequestGraphicsPipeline(GraphicsPipelineRequest{
        .VertEntry = {.SourcePath = Path("DefinitelyMissingShader.slang"), .EntryPoint = "vertMain"},
        .FragEntry = {.SourcePath = Path("DefinitelyMissingShader.slang"), .EntryPoint = "fragMain"},
    });

    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (IsPending(Handle.GetState()) && std::chrono::steady_clock::now() < Deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    EXPECT_EQ(Handle.GetState(), ResourceState::Failed);
    ASSERT_TRUE(Handle.GetError().has_value());

    Graph.Shutdown();
    Manager::Get().Clear();
}
