/// @file   ResourceHandle.cpp
/// @brief  Tests for asynchronous resource handle publication.

#include <gtest/gtest.h>

import Resource;
import TaskGraph;
import std;

using namespace SoulEngine::Core;
using namespace SoulEngine;
using namespace SoulEngine::Resource;

TEST(ResourceHandleTest, ReadyPublishMakesHandleReady) {
    auto Resource = CreatePendingResource<TextureResource>(42);

    EXPECT_EQ(Resource.Handle.GetState(), ResourceState::Loading);
    EXPECT_TRUE(Resource.Slot->PublishReady(Resource.Handle.GetGeneration(), TextureResource{}));

    EXPECT_EQ(Resource.Handle.GetState(), ResourceState::Ready);
    EXPECT_TRUE(Resource.Handle.IsReady());
    EXPECT_TRUE(Resource.Handle.TryGet().has_value());
}

TEST(ResourceHandleTest, FailedPublishMakesHandleFailed) {
    auto Resource = CreatePendingResource<TextureResource>(43);

    EXPECT_TRUE(Resource.Slot->PublishFailed(Resource.Handle.GetGeneration(), ErrorMessage{"missing texture"}));

    EXPECT_EQ(Resource.Handle.GetState(), ResourceState::Failed);
    EXPECT_TRUE(Resource.Handle.IsFailed());
    ASSERT_TRUE(Resource.Handle.GetError().has_value());
    EXPECT_TRUE(Resource.Handle.GetError()->ToString().starts_with("missing texture"));
}

TEST(ResourceHandleTest, StaleGenerationPublishIgnored) {
    auto Resource         = CreatePendingResource<TextureResource>(44);
    auto FirstGeneration  = Resource.Handle.GetGeneration();
    auto SecondGeneration = Resource.Slot->Reset(Resource.Handle.GetId());
    auto CurrentHandle =
        ResourceHandle<TextureResource>::Create(Resource.Slot, Resource.Handle.GetId(), SecondGeneration);

    EXPECT_FALSE(Resource.Slot->PublishReady(FirstGeneration, TextureResource{}));
    EXPECT_EQ(Resource.Handle.GetState(), ResourceState::Stale);
    EXPECT_EQ(CurrentHandle.GetState(), ResourceState::Loading);

    EXPECT_TRUE(Resource.Slot->PublishReady(SecondGeneration, TextureResource{}));
    EXPECT_EQ(CurrentHandle.GetState(), ResourceState::Ready);
}

TEST(ResourceHandleTest, DefaultHandleStateIsInvalid) {
    TextureHandle Handle;

    EXPECT_FALSE(Handle.IsValid());
    EXPECT_EQ(Handle.GetState(), ResourceState::Invalid);
    EXPECT_FALSE(Handle.TryGet().has_value());
}

TEST(ResourceTextureRequestTest, CoalescesNormalizedTexturePaths) {
    TaskGraph Graph;
    Manager::Get().Clear();
    Manager::Get().Init(Graph);

    auto A = Manager::Get().RequestTexture("Assets/../Textures/Missing.png");
    auto B = Manager::Get().RequestTexture("Textures/Missing.png");

    EXPECT_TRUE(A.IsValid());
    EXPECT_EQ(A.GetId(), B.GetId());
    EXPECT_EQ(A.GetGeneration(), B.GetGeneration());

    Graph.Shutdown();
    Manager::Get().Clear();
}

TEST(ResourceTextureRequestTest, RejectsRequestsAfterShutdownBegins) {
    TaskGraph Graph;
    Manager::Get().Clear();
    Manager::Get().Init(Graph);
    Manager::Get().BeginShutdown();

    auto Handle = Manager::Get().RequestTexture("Textures/Missing.png");

    EXPECT_FALSE(Handle.IsValid());

    Graph.Shutdown();
    Manager::Get().Clear();
    Manager::Get().Init(Graph);
}

TEST(ResourceTextureRequestTest, MissingFilePublishesFailedResource) {
    TaskGraph Graph;
    Graph.Init(1);
    Manager::Get().Clear();
    Manager::Get().Init(Graph);

    auto Handle = Manager::Get().RequestTexture("DefinitelyMissingTexture.png");

    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (Handle.GetState() == ResourceState::Loading && std::chrono::steady_clock::now() < Deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    EXPECT_EQ(Handle.GetState(), ResourceState::Failed);
    ASSERT_TRUE(Handle.GetError().has_value());
    EXPECT_TRUE(Handle.GetError()->ToString().starts_with("stbi_load failed"));

    Graph.Shutdown();
    Manager::Get().Clear();
}

TEST(ResourceTextureRequestTest, StaleTexturePublishIgnored) {
    auto Resource        = CreatePendingResource<TextureResource>(45);
    auto FirstGeneration = Resource.Handle.GetGeneration();
    auto NextGeneration  = Resource.Slot->Reset(Resource.Handle.GetId());

    EXPECT_FALSE(Resource.Slot->PublishFailed(FirstGeneration, ErrorMessage{"old failure"}));
    EXPECT_EQ(Resource.Handle.GetState(), ResourceState::Stale);
    EXPECT_EQ(Resource.Slot->GetState(NextGeneration), ResourceState::Loading);
}

TEST(ResourcePipelineRequestTest, CoalescesPipelineKeys) {
    TaskGraph Graph;
    Manager::Get().Clear();
    Manager::Get().Init(Graph);

    GraphicsPipelineRequest Req{
        .VertEntry = {.ShaderPath = Path("Shaders/Test.slang"), .EntryName = "vertMain"},
        .FragEntry = {.ShaderPath = Path("Shaders/Test.slang"), .EntryName = "fragMain"},
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
        .VertEntry = {.ShaderPath = Path("DefinitelyMissingShader.slang"), .EntryName = "vertMain"},
        .FragEntry = {.ShaderPath = Path("DefinitelyMissingShader.slang"), .EntryName = "fragMain"},
    });

    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (Handle.GetState() == ResourceState::Loading && std::chrono::steady_clock::now() < Deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    EXPECT_EQ(Handle.GetState(), ResourceState::Failed);
    ASSERT_TRUE(Handle.GetError().has_value());

    Graph.Shutdown();
    Manager::Get().Clear();
}
