/// @file   ResourceHandle.cpp
/// @brief  Tests for ResourceContext-owned slots, handles, and pins.

#include <gtest/gtest.h>

import Resource;
import TaskGraph;
import std;

using namespace SoulEngine::Core;
using namespace SoulEngine;
using namespace SoulEngine::Resource;

namespace {

[[nodiscard]] auto IsPending(ResourceState State) -> bool {
    return State == ResourceState::CpuPreparing || State == ResourceState::RhiCommitting ||
           State == ResourceState::GpuPending;
}

class MockSampledTexture final : public RHI::SampledTexture {
  public:
    MockSampledTexture() = default;
    explicit MockSampledTexture(bool* Destroyed) : m_Destroyed(Destroyed) {}

    ~MockSampledTexture() override {
        if (m_Destroyed)
            *m_Destroyed = true;
    }

    [[nodiscard]] auto GetWidth() const -> Uint32 override {
        return 64;
    }

    [[nodiscard]] auto GetHeight() const -> Uint32 override {
        return 64;
    }

  private:
    bool* m_Destroyed = nullptr;
};

[[nodiscard]] auto MakeTextureResource(bool* Destroyed = nullptr)
    -> SoulEngine::Resource::Resource<RHI::SampledTexture> {
    return SoulEngine::Resource::Resource<RHI::SampledTexture>{
        .Object = std::make_unique<MockSampledTexture>(Destroyed),
    };
}

auto ResetManagerForTest() -> void {
    Manager::Get().BeginShutdown();
    Manager::Get().Clear();
}

[[nodiscard]] auto MakeTestRenderTargetDesc() -> RHI::RenderTargetDesc {
    return RHI::RenderTargetDesc{
        .Width  = 128,
        .Height = 64,
        .Format = RHI::Format::D32_SFLOAT,
        .Usage  = RHI::TextureUsage::DepthStencil,
    };
}

} // namespace

TEST(ResourceSlotTest, ReadyPublishMakesSlotReadyAndPinnable) {
    ResourceSlot<RHI::SampledTexture> Slot;
    auto Generation = Slot.Reset();

    EXPECT_EQ(Slot.GetState(Generation), ResourceState::CpuPreparing);
    EXPECT_TRUE(Slot.PublishReady(Generation, MakeTextureResource()));

    EXPECT_EQ(Slot.GetState(Generation), ResourceState::Ready);
    auto Pin = Slot.PinReady(Generation);
    EXPECT_TRUE(Pin);
    EXPECT_NE(Pin.Get(), nullptr);
}

TEST(ResourceSlotTest, FailedPublishMakesSlotFailed) {
    ResourceSlot<RHI::SampledTexture> Slot;
    auto Generation = Slot.Reset();

    EXPECT_TRUE(Slot.PublishFailed(Generation, ErrorMessage{"missing texture"}));

    EXPECT_EQ(Slot.GetState(Generation), ResourceState::Failed);
    ASSERT_TRUE(Slot.GetError(Generation).has_value());
    EXPECT_TRUE(Slot.GetError(Generation)->ToString().starts_with("missing texture"));
}

TEST(ResourceSlotTest, StaleGenerationPublishIgnored) {
    ResourceSlot<RHI::SampledTexture> Slot;
    auto FirstGeneration  = Slot.Reset();
    auto SecondGeneration = Slot.Reset();

    EXPECT_FALSE(Slot.PublishReady(FirstGeneration, MakeTextureResource()));
    EXPECT_EQ(Slot.GetState(FirstGeneration), ResourceState::Stale);
    EXPECT_EQ(Slot.GetState(SecondGeneration), ResourceState::CpuPreparing);

    EXPECT_TRUE(Slot.PublishReady(SecondGeneration, MakeTextureResource()));
    EXPECT_EQ(Slot.GetState(SecondGeneration), ResourceState::Ready);
}

TEST(ResourceHandleTest, DefaultHandleStateIsUnknown) {
    ResourceHandle<RHI::SampledTexture> Handle;
    ResourceRef<RHI::SampledTexture> Ref;

    EXPECT_FALSE(Handle.IsValid());
    EXPECT_EQ(Manager::Get().GetState(Handle), ResourceState::Unknown);
    FrameResourceScope Scope;
    EXPECT_EQ(Scope.Acquire(Handle), nullptr);
    EXPECT_EQ(Scope.Acquire(Ref), nullptr);
}

TEST(ResourceSlotTest, ExplicitStateTransitionsPublishExpectedStates) {
    ResourceSlot<RHI::SampledTexture> Slot;
    auto Generation = Slot.Reset();

    EXPECT_EQ(Slot.GetState(Generation), ResourceState::CpuPreparing);
    EXPECT_TRUE(Slot.MarkRhiCommitting(Generation));
    EXPECT_EQ(Slot.GetState(Generation), ResourceState::RhiCommitting);
    EXPECT_TRUE(Slot.PublishGpuPending(Generation));
    EXPECT_EQ(Slot.GetState(Generation), ResourceState::GpuPending);
    EXPECT_TRUE(Slot.PublishReady(Generation, MakeTextureResource()));
    EXPECT_EQ(Slot.GetState(Generation), ResourceState::Ready);
}

TEST(ResourceSlotTest, StaleGenerationCannotPublishPendingReadyOrFailed) {
    ResourceSlot<RHI::SampledTexture> Slot;
    auto FirstGeneration  = Slot.Reset();
    auto SecondGeneration = Slot.Reset();

    EXPECT_FALSE(Slot.MarkRhiCommitting(FirstGeneration));
    EXPECT_FALSE(Slot.PublishGpuPending(FirstGeneration));
    EXPECT_FALSE(Slot.PublishReady(FirstGeneration, MakeTextureResource()));
    EXPECT_FALSE(Slot.PublishFailed(FirstGeneration, ErrorMessage{"old failure"}));
    EXPECT_EQ(Slot.GetState(FirstGeneration), ResourceState::Stale);
    EXPECT_EQ(Slot.GetState(SecondGeneration), ResourceState::CpuPreparing);
}

TEST(ResourcePinTest, PinReadyOnlySucceedsForReadyMatchingGeneration) {
    ResourceSlot<RHI::SampledTexture> Slot;
    auto FirstGeneration = Slot.Reset();

    EXPECT_FALSE(Slot.PinReady(FirstGeneration));

    EXPECT_TRUE(Slot.PublishReady(FirstGeneration, MakeTextureResource()));
    EXPECT_TRUE(Slot.PinReady(FirstGeneration));

    auto SecondGeneration = Slot.Reset();
    EXPECT_FALSE(Slot.PinReady(FirstGeneration));
    EXPECT_FALSE(Slot.PinReady(SecondGeneration));
}

TEST(ResourcePinTest, ReleaseDefersPayloadReleaseUntilPinDestructs) {
    ResourceSlot<RHI::SampledTexture> Slot;
    auto Generation = Slot.Reset();
    bool Destroyed  = false;
    EXPECT_TRUE(Slot.PublishReady(Generation, MakeTextureResource(&Destroyed)));

    {
        auto Pin = Slot.PinReady(Generation);
        ASSERT_TRUE(Pin);
        ASSERT_FALSE(Destroyed);

        Slot.RequestRelease();

        EXPECT_EQ(Slot.GetState(Generation), ResourceState::Stale);
        EXPECT_FALSE(Destroyed);
    }

    EXPECT_TRUE(Destroyed);
}

TEST(ResourcePinTest, ResetRetiresPinnedPayloadUntilPinDestructs) {
    ResourceSlot<RHI::SampledTexture> Slot;
    auto FirstGeneration = Slot.Reset();
    bool OldDestroyed    = false;
    bool NewDestroyed    = false;
    EXPECT_TRUE(Slot.PublishReady(FirstGeneration, MakeTextureResource(&OldDestroyed)));

    {
        auto Pin = Slot.PinReady(FirstGeneration);
        ASSERT_TRUE(Pin);

        auto SecondGeneration = Slot.Reset();
        EXPECT_EQ(Slot.GetState(FirstGeneration), ResourceState::Stale);
        EXPECT_EQ(Slot.GetState(SecondGeneration), ResourceState::CpuPreparing);
        EXPECT_FALSE(OldDestroyed);

        EXPECT_TRUE(Slot.PublishReady(SecondGeneration, MakeTextureResource(&NewDestroyed)));
        EXPECT_EQ(Slot.GetState(SecondGeneration), ResourceState::Ready);
        EXPECT_FALSE(OldDestroyed);
        EXPECT_FALSE(NewDestroyed);
    }

    EXPECT_TRUE(OldDestroyed);
    EXPECT_FALSE(NewDestroyed);

    Slot.RequestRelease();
    EXPECT_TRUE(NewDestroyed);
}

TEST(ResourceSlotTest, RequestReleaseDestroysPayload) {
    ResourceSlot<RHI::SampledTexture> Slot;
    auto Generation = Slot.Reset();
    bool Destroyed  = false;
    EXPECT_TRUE(Slot.PublishReady(Generation, MakeTextureResource(&Destroyed)));

    Slot.RequestRelease();
    EXPECT_EQ(Slot.GetState(Generation), ResourceState::Stale);
    EXPECT_TRUE(Destroyed);
}

TEST(ResourceRefTest, RequestRefCreatesLogicalOwnerHandle) {
    ResetManagerForTest();
    TaskGraph Graph;
    Graph.Init(1);
    Manager::Get().Init(Graph);

    auto Ref    = Manager::Get().RequestSampledTextureRef("DefinitelyMissingRefTexture.png");
    auto Handle = Ref.GetHandle();
    ASSERT_TRUE(Handle.IsValid());

    {
        auto MovedRef = std::move(Ref);
        EXPECT_TRUE(MovedRef);
        EXPECT_FALSE(Ref);
    }

    Graph.Shutdown();
    Manager::Get().Clear();
}

TEST(ResourceRefLifetimeTest, CachedAssetLastRefReleaseDoesNotMakeEntryStale) {
    ResetManagerForTest();
    TaskGraph Graph;
    Manager::Get().Init(Graph);

    auto Ref    = Manager::Get().RequestSampledTextureRef("CachedRefLifetimeTexture.png");
    auto Handle = Ref.GetHandle();
    ASSERT_TRUE(Handle.IsValid());
    EXPECT_EQ(Manager::Get().GetState(Handle), ResourceState::CpuPreparing);

    Ref.Reset();

    EXPECT_EQ(Manager::Get().GetState(Handle), ResourceState::CpuPreparing);

    Graph.Shutdown();
    Manager::Get().Clear();
}

TEST(ResourceRefLifetimeTest, TransientLastRefReleaseMakesEntryStale) {
    ResetManagerForTest();
    TaskGraph Graph;
    Manager::Get().Init(Graph);

    auto Ref    = Manager::Get().RequestRenderTargetRef("TransientRefLifetimeDepth", MakeTestRenderTargetDesc());
    auto Handle = Ref.GetHandle();
    ASSERT_TRUE(Handle.IsValid());
    EXPECT_EQ(Manager::Get().GetState(Handle), ResourceState::CpuPreparing);

    Ref.Reset();

    EXPECT_EQ(Manager::Get().GetState(Handle), ResourceState::Stale);

    Graph.Shutdown();
    Manager::Get().Clear();
}

TEST(ResourceRefLifetimeTest, MoveAssignmentReleasesPreviousTransientOwner) {
    ResetManagerForTest();
    TaskGraph Graph;
    Manager::Get().Init(Graph);

    auto FirstRef    = Manager::Get().RequestRenderTargetRef("TransientMoveAssignFirst", MakeTestRenderTargetDesc());
    auto FirstHandle = FirstRef.GetHandle();
    auto NextRef     = Manager::Get().RequestRenderTargetRef("TransientMoveAssignNext", MakeTestRenderTargetDesc());
    auto NextHandle  = NextRef.GetHandle();
    ASSERT_TRUE(FirstHandle.IsValid());
    ASSERT_TRUE(NextHandle.IsValid());

    FirstRef = std::move(NextRef);

    EXPECT_FALSE(NextRef);
    EXPECT_EQ(Manager::Get().GetState(FirstHandle), ResourceState::Stale);
    EXPECT_EQ(Manager::Get().GetState(NextHandle), ResourceState::CpuPreparing);

    FirstRef.Reset();
    EXPECT_EQ(Manager::Get().GetState(NextHandle), ResourceState::Stale);

    Graph.Shutdown();
    Manager::Get().Clear();
}

TEST(ResourceRefLifetimeTest, CollectReleasedResourcesErasesReleasedTransientEntry) {
    ResetManagerForTest();
    TaskGraph Graph;
    Manager::Get().Init(Graph);

    auto Ref    = Manager::Get().RequestRenderTargetRef("TransientCollectReleased", MakeTestRenderTargetDesc());
    auto Handle = Ref.GetHandle();
    ASSERT_TRUE(Handle.IsValid());

    Ref.Reset();
    EXPECT_EQ(Manager::Get().GetState(Handle), ResourceState::Stale);

    Manager::Get().CollectReleasedResources();
    EXPECT_EQ(Manager::Get().GetState(Handle), ResourceState::Unknown);

    Graph.Shutdown();
    Manager::Get().Clear();
}

TEST(ResourceSampledTextureRequestTest, CoalescesNormalizedTexturePaths) {
    ResetManagerForTest();
    TaskGraph Graph;
    Manager::Get().Init(Graph);

    auto ARef = Manager::Get().RequestSampledTextureRef("Assets/../Textures/Missing.png");
    auto BRef = Manager::Get().RequestSampledTextureRef("Textures/Missing.png");
    auto A    = ARef.GetHandle();
    auto B    = BRef.GetHandle();

    EXPECT_TRUE(A.IsValid());
    EXPECT_EQ(A.GetKey(), B.GetKey());
    EXPECT_EQ(A.GetGeneration(), B.GetGeneration());

    Graph.Shutdown();
    Manager::Get().Clear();
}

TEST(ResourceSampledTextureRequestTest, RejectsRequestsAfterShutdownBegins) {
    ResetManagerForTest();
    TaskGraph Graph;
    Manager::Get().Init(Graph);
    Manager::Get().BeginShutdown();

    auto Ref    = Manager::Get().RequestSampledTextureRef("Textures/Missing.png");
    auto Handle = Ref.GetHandle();

    EXPECT_FALSE(Handle.IsValid());

    Graph.Shutdown();
    Manager::Get().Clear();
    Manager::Get().Init(Graph);
}

TEST(ResourceSampledTextureRequestTest, MissingFilePublishesFailedResource) {
    ResetManagerForTest();
    TaskGraph Graph;
    Graph.Init(1);
    Manager::Get().Init(Graph);

    auto Ref    = Manager::Get().RequestSampledTextureRef("DefinitelyMissingTexture.png");
    auto Handle = Ref.GetHandle();

    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (IsPending(Manager::Get().GetState(Handle)) && std::chrono::steady_clock::now() < Deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    EXPECT_EQ(Manager::Get().GetState(Handle), ResourceState::Failed);
    ASSERT_TRUE(Manager::Get().GetError(Handle).has_value());
    EXPECT_TRUE(Manager::Get().GetError(Handle)->ToString().starts_with("stbi_load failed"));

    Graph.Shutdown();
    Manager::Get().Clear();
}

TEST(ResourceSampledTextureRequestTest, StaleTexturePublishIgnored) {
    ResourceSlot<RHI::SampledTexture> Slot;
    auto FirstGeneration  = Slot.Reset();
    auto SecondGeneration = Slot.Reset();

    EXPECT_FALSE(Slot.PublishFailed(FirstGeneration, ErrorMessage{"old failure"}));
    EXPECT_EQ(Slot.GetState(FirstGeneration), ResourceState::Stale);
    EXPECT_EQ(Slot.GetState(SecondGeneration), ResourceState::CpuPreparing);
}

TEST(ResourcePipelineRequestTest, CoalescesPipelineKeys) {
    ResetManagerForTest();
    TaskGraph Graph;
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

    auto ARef = Manager::Get().RequestGraphicsPipelineRef(Req);
    auto BRef = Manager::Get().RequestGraphicsPipelineRef(Req);
    auto A    = ARef.GetHandle();
    auto B    = BRef.GetHandle();

    EXPECT_TRUE(A.IsValid());
    EXPECT_EQ(A.GetKey(), B.GetKey());
    EXPECT_EQ(A.GetGeneration(), B.GetGeneration());

    Graph.Shutdown();
    Manager::Get().Clear();
}

TEST(ResourcePipelineRequestTest, ShaderCompileFailurePublishesFailedResource) {
    ResetManagerForTest();
    TaskGraph Graph;
    Graph.Init(1);
    Manager::Get().Init(Graph);

    auto Ref = Manager::Get().RequestGraphicsPipelineRef(GraphicsPipelineRequest{
        .VertEntry = {.SourcePath = Path("DefinitelyMissingShader.slang"), .EntryPoint = "vertMain"},
        .FragEntry = {.SourcePath = Path("DefinitelyMissingShader.slang"), .EntryPoint = "fragMain"},
    });
    auto Handle = Ref.GetHandle();

    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (IsPending(Manager::Get().GetState(Handle)) && std::chrono::steady_clock::now() < Deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    EXPECT_EQ(Manager::Get().GetState(Handle), ResourceState::Failed);
    ASSERT_TRUE(Manager::Get().GetError(Handle).has_value());

    Graph.Shutdown();
    Manager::Get().Clear();
}
