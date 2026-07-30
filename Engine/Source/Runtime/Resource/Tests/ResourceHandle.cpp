/// @file   ResourceHandle.cpp
/// @brief  Tests for ResourceContext-owned slots and handles.

#include <gtest/gtest.h>

import Resource;
import TaskGraph;
import std;

using namespace SoulEngine;

namespace {

[[nodiscard]] auto IsPending(ResourceState State) -> bool {
    return State == ResourceState::CpuPreparing || State == ResourceState::RhiCommitting ||
           State == ResourceState::GpuPending;
}

class MockSampledTexture final : public RHISampledTexture {
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
    -> Resource<RHISampledTexture> {
    return Resource<RHISampledTexture>{
        .Object = std::make_unique<MockSampledTexture>(Destroyed),
    };
}

[[nodiscard]] auto MakeTestRenderTargetDesc() -> RHIRenderTargetDesc {
    return RHIRenderTargetDesc{
        .Width  = 128,
        .Height = 64,
        .Format = RHIFormat::D32_SFLOAT,
        .Usage  = RHITextureUsage::DepthStencil,
    };
}

} // namespace

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

TEST(ResourceSlotTest, ReadyPublishMakesSlotReadyAndReadable) {
    ResourceSlot<RHISampledTexture> Slot;
    auto Generation = Slot.Reset();

    EXPECT_EQ(Slot.GetState(Generation), ResourceState::CpuPreparing);
    EXPECT_TRUE(Slot.PublishReady(Generation, MakeTextureResource()));

    EXPECT_EQ(Slot.GetState(Generation), ResourceState::Ready);
    EXPECT_NE(Slot.TryGetReady(Generation), nullptr);
}

TEST(ResourceSlotTest, FailedPublishMakesSlotFailed) {
    ResourceSlot<RHISampledTexture> Slot;
    auto Generation = Slot.Reset();

    EXPECT_TRUE(Slot.PublishFailed(Generation, ErrorMessage{"missing texture"}));

    EXPECT_EQ(Slot.GetState(Generation), ResourceState::Failed);
    ASSERT_TRUE(Slot.GetError(Generation).has_value());
    EXPECT_TRUE(Slot.GetError(Generation)->ToString().starts_with("missing texture"));
}

TEST(ResourceSlotTest, StaleGenerationPublishIgnored) {
    ResourceSlot<RHISampledTexture> Slot;
    auto FirstGeneration  = Slot.Reset();
    auto SecondGeneration = Slot.Reset();

    EXPECT_FALSE(Slot.PublishReady(FirstGeneration, MakeTextureResource()));
    EXPECT_EQ(Slot.GetState(FirstGeneration), ResourceState::Stale);
    EXPECT_EQ(Slot.GetState(SecondGeneration), ResourceState::CpuPreparing);

    EXPECT_TRUE(Slot.PublishReady(SecondGeneration, MakeTextureResource()));
    EXPECT_EQ(Slot.GetState(SecondGeneration), ResourceState::Ready);
}

TEST(ResourceHandleTest, DefaultHandleStateIsUnknown) {
    ResourceHandle<RHISampledTexture> Handle;
    ResourceRef<RHISampledTexture> Ref;

    EXPECT_FALSE(Handle.IsValid());
    EXPECT_EQ(ResourceManager::Get().GetState(Handle), ResourceState::Unknown);
    EXPECT_EQ(ResourceManager::Get().TryGetReady(Handle), nullptr);
    EXPECT_EQ(ResourceManager::Get().TryGetReady(Ref), nullptr);
}

TEST(ResourceSlotTest, ExplicitStateTransitionsPublishExpectedStates) {
    ResourceSlot<RHISampledTexture> Slot;
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
    ResourceSlot<RHISampledTexture> Slot;
    auto FirstGeneration  = Slot.Reset();
    auto SecondGeneration = Slot.Reset();

    EXPECT_FALSE(Slot.MarkRhiCommitting(FirstGeneration));
    EXPECT_FALSE(Slot.PublishGpuPending(FirstGeneration));
    EXPECT_FALSE(Slot.PublishReady(FirstGeneration, MakeTextureResource()));
    EXPECT_FALSE(Slot.PublishFailed(FirstGeneration, ErrorMessage{"old failure"}));
    EXPECT_EQ(Slot.GetState(FirstGeneration), ResourceState::Stale);
    EXPECT_EQ(Slot.GetState(SecondGeneration), ResourceState::CpuPreparing);
}

TEST(ResourceSlotTest, TryGetReadyOnlySucceedsForReadyMatchingGeneration) {
    ResourceSlot<RHISampledTexture> Slot;
    auto FirstGeneration = Slot.Reset();

    EXPECT_EQ(Slot.TryGetReady(FirstGeneration), nullptr);

    EXPECT_TRUE(Slot.PublishReady(FirstGeneration, MakeTextureResource()));
    EXPECT_NE(Slot.TryGetReady(FirstGeneration), nullptr);

    auto SecondGeneration = Slot.Reset();
    EXPECT_EQ(Slot.TryGetReady(FirstGeneration), nullptr);
    EXPECT_EQ(Slot.TryGetReady(SecondGeneration), nullptr);
}

TEST(ResourceSlotTest, RequestReleaseDestroysPayload) {
    ResourceSlot<RHISampledTexture> Slot;
    auto Generation = Slot.Reset();
    bool Destroyed  = false;
    EXPECT_TRUE(Slot.PublishReady(Generation, MakeTextureResource(&Destroyed)));

    Slot.RequestRelease();
    EXPECT_EQ(Slot.GetState(Generation), ResourceState::Stale);
    EXPECT_TRUE(Destroyed);
}

TEST_F(ResourceManagerTest, RequestRefCreatesLogicalOwnerHandle) {
    auto Ref    = ResourceManager::Get().RequestRenderTargetRef("TransientRefOwnerHandle", MakeTestRenderTargetDesc());
    auto Handle = Ref.GetHandle();
    ASSERT_TRUE(Handle.IsValid());

    {
        auto MovedRef = std::move(Ref);
        EXPECT_TRUE(MovedRef);
        EXPECT_FALSE(Ref);
    }
}

TEST_F(ResourceManagerTest, EquivalentMeshBlasRequestsDeduplicateBeforeDependenciesAreReady) {
    auto MeshRef = ResourceManager::Get().RequestMeshRef("NotYetLoadedMesh.obj");
    ASSERT_TRUE(MeshRef);
    auto FirstRef = ResourceManager::Get().RequestBottomLevelAccelerationStructureRef(MeshRef);
    auto SecondRef = ResourceManager::Get().RequestBottomLevelAccelerationStructureRef(MeshRef);
    ASSERT_TRUE(FirstRef);
    ASSERT_TRUE(SecondRef);

    const auto& First = FirstRef.GetHandle();
    const auto& Second = SecondRef.GetHandle();
    EXPECT_EQ(First.GetKey(), Second.GetKey());
    EXPECT_EQ(First.GetGeneration(), Second.GetGeneration());
    EXPECT_EQ(ResourceManager::Get().GetState(First), ResourceState::CpuPreparing);
}

TEST_F(ResourceManagerTest, RendererScopedTlasRequestsDeduplicateAndReleaseAsTransient) {
    const RHITopLevelAccelerationStructureDesc Desc{.InitialInstanceCapacity = 4};
    auto FirstRef = ResourceManager::Get().RequestTopLevelAccelerationStructureRef("RayTracingRendererMain", Desc);
    auto SecondRef = ResourceManager::Get().RequestTopLevelAccelerationStructureRef("RayTracingRendererMain", Desc);
    auto OtherScopeRef = ResourceManager::Get().RequestTopLevelAccelerationStructureRef("RayTracingRendererReflection", Desc);
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

TEST_F(ResourceManagerTest, CachedAssetLastRefReleaseDoesNotMakeEntryStale) {
    auto Ref    = ResourceManager::Get().RequestSampledTextureRef("CachedRefLifetimeTexture.png");
    auto Handle = Ref.GetHandle();
    ASSERT_TRUE(Handle.IsValid());
    EXPECT_EQ(ResourceManager::Get().GetState(Handle), ResourceState::CpuPreparing);

    Ref.Reset();

    EXPECT_EQ(ResourceManager::Get().GetState(Handle), ResourceState::CpuPreparing);
}

TEST_F(ResourceManagerTest, TransientLastRefReleaseMakesEntryStale) {
    auto Ref    = ResourceManager::Get().RequestRenderTargetRef("TransientRefLifetimeDepth", MakeTestRenderTargetDesc());
    auto Handle = Ref.GetHandle();
    ASSERT_TRUE(Handle.IsValid());
    EXPECT_EQ(ResourceManager::Get().GetState(Handle), ResourceState::CpuPreparing);

    Ref.Reset();

    EXPECT_EQ(ResourceManager::Get().GetState(Handle), ResourceState::Stale);
}

TEST_F(ResourceManagerTest, MoveAssignmentReleasesPreviousTransientOwner) {
    auto FirstRef    = ResourceManager::Get().RequestRenderTargetRef("TransientMoveAssignFirst", MakeTestRenderTargetDesc());
    auto FirstHandle = FirstRef.GetHandle();
    auto NextRef     = ResourceManager::Get().RequestRenderTargetRef("TransientMoveAssignNext", MakeTestRenderTargetDesc());
    auto NextHandle  = NextRef.GetHandle();
    ASSERT_TRUE(FirstHandle.IsValid());
    ASSERT_TRUE(NextHandle.IsValid());

    FirstRef = std::move(NextRef);

    EXPECT_FALSE(NextRef);
    EXPECT_EQ(ResourceManager::Get().GetState(FirstHandle), ResourceState::Stale);
    EXPECT_EQ(ResourceManager::Get().GetState(NextHandle), ResourceState::CpuPreparing);

    FirstRef.Reset();
    EXPECT_EQ(ResourceManager::Get().GetState(NextHandle), ResourceState::Stale);
}

TEST_F(ResourceManagerTest, CollectReleasedResourcesErasesReleasedTransientEntry) {
    auto Ref    = ResourceManager::Get().RequestRenderTargetRef("TransientCollectReleased", MakeTestRenderTargetDesc());
    auto Handle = Ref.GetHandle();
    ASSERT_TRUE(Handle.IsValid());

    Ref.Reset();
    EXPECT_EQ(ResourceManager::Get().GetState(Handle), ResourceState::Stale);

    ResourceManager::Get().CollectReleasedResources();
    EXPECT_EQ(ResourceManager::Get().GetState(Handle), ResourceState::Unknown);
}

TEST_F(ResourceManagerTest, CoalescesNormalizedTexturePaths) {
    auto ARef = ResourceManager::Get().RequestSampledTextureRef("Assets/../Textures/Missing.png");
    auto BRef = ResourceManager::Get().RequestSampledTextureRef("Textures/Missing.png");
    auto A    = ARef.GetHandle();
    auto B    = BRef.GetHandle();

    EXPECT_TRUE(A.IsValid());
    EXPECT_EQ(A.GetKey(), B.GetKey());
    EXPECT_EQ(A.GetGeneration(), B.GetGeneration());
}

TEST_F(ResourceManagerTest, RejectsRequestsAfterShutdownBegins) {
    ResourceManager::Get().BeginShutdown();

    auto Ref    = ResourceManager::Get().RequestSampledTextureRef("Textures/Missing.png");
    auto Handle = Ref.GetHandle();

    EXPECT_FALSE(Handle.IsValid());
}

TEST_F(ResourceManagerTest, MissingFilePublishesFailedResource) {
    auto Ref    = ResourceManager::Get().RequestSampledTextureRef("DefinitelyMissingTexture.png");
    auto Handle = Ref.GetHandle();

    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (IsPending(ResourceManager::Get().GetState(Handle)) && std::chrono::steady_clock::now() < Deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    EXPECT_EQ(ResourceManager::Get().GetState(Handle), ResourceState::Failed);
    ASSERT_TRUE(ResourceManager::Get().GetError(Handle).has_value());
    EXPECT_TRUE(ResourceManager::Get().GetError(Handle)->ToString().starts_with("stbi_load failed"));
}

TEST(ResourceSampledTextureSlotTest, StaleTexturePublishIgnored) {
    ResourceSlot<RHISampledTexture> Slot;
    auto FirstGeneration  = Slot.Reset();
    auto SecondGeneration = Slot.Reset();

    EXPECT_FALSE(Slot.PublishFailed(FirstGeneration, ErrorMessage{"old failure"}));
    EXPECT_EQ(Slot.GetState(FirstGeneration), ResourceState::Stale);
    EXPECT_EQ(Slot.GetState(SecondGeneration), ResourceState::CpuPreparing);
}

TEST(ResourceArrayTest, RejectsInvalidResourceRef) {
    ResourceArray<RHISampledTexture> Resources;

    auto Result = Resources.Set(0, {});

    ASSERT_FALSE(Result.has_value());
    EXPECT_NE(Result.error().ToString().find("invalid resource ref"), String::npos);
}

TEST_F(ResourceManagerTest, CoalescesPipelineKeys) {
    GraphicsPipelineRequest Req{
        .VertEntry = {.SourcePath = Path("Shaders/Test.slang"), .EntryPoint = "vertMain"},
        .FragEntry = {.SourcePath = Path("Shaders/Test.slang"), .EntryPoint = "fragMain"},
        .VertexInputLayout =
            RHIVertexInputLayoutDesc{
                .Bindings =
                    {
                        {.Binding = 0, .Stride = 32},
                    },
                .Attributes =
                    {
                        RHIVertexInputAttributeDesc{
                            .Location = 0,
                            .Binding  = 0,
                            .Format   = RHIFormat::R32G32B32_SFLOAT,
                            .Offset   = 0,
                        },
                    },
            },
    };

    auto ARef = ResourceManager::Get().RequestGraphicsPipelineRef(Req);
    auto BRef = ResourceManager::Get().RequestGraphicsPipelineRef(Req);
    auto A    = ARef.GetHandle();
    auto B    = BRef.GetHandle();

    EXPECT_TRUE(A.IsValid());
    EXPECT_EQ(A.GetKey(), B.GetKey());
    EXPECT_EQ(A.GetGeneration(), B.GetGeneration());
}

TEST_F(ResourceManagerTest, ShaderCompileFailurePublishesFailedResource) {
    auto Ref = ResourceManager::Get().RequestGraphicsPipelineRef(GraphicsPipelineRequest{
        .VertEntry = {.SourcePath = Path("DefinitelyMissingShader.slang"), .EntryPoint = "vertMain"},
        .FragEntry = {.SourcePath = Path("DefinitelyMissingShader.slang"), .EntryPoint = "fragMain"},
    });
    auto Handle = Ref.GetHandle();

    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (IsPending(ResourceManager::Get().GetState(Handle)) && std::chrono::steady_clock::now() < Deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    EXPECT_EQ(ResourceManager::Get().GetState(Handle), ResourceState::Failed);
    ASSERT_TRUE(ResourceManager::Get().GetError(Handle).has_value());
}
