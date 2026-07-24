/// @file   ResourceHandle.cpp
/// @brief  Tests for ResourceContext-owned slots and handles.

#include <gtest/gtest.h>

#include <GLFW/glfw3.h>

import Resource;
import TaskGraph;
import Vulkan;
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

auto ResetManagerForTest() -> void {
    ResourceManager::Get().BeginShutdown();
    ResourceManager::Get().Clear();
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

TEST(ResourceRefTest, RequestRefCreatesLogicalOwnerHandle) {
    ResetManagerForTest();
    TaskGraph Graph;
    ResourceManager::Get().Init(Graph);

    auto Ref    = ResourceManager::Get().RequestRenderTargetRef("TransientRefOwnerHandle", MakeTestRenderTargetDesc());
    auto Handle = Ref.GetHandle();
    ASSERT_TRUE(Handle.IsValid());

    {
        auto MovedRef = std::move(Ref);
        EXPECT_TRUE(MovedRef);
        EXPECT_FALSE(Ref);
    }

    Graph.Shutdown();
    ResourceManager::Get().Clear();
}

TEST(ResourceAccelerationStructureRequestTest, EquivalentMeshBlasRequestsDeduplicateBeforeDependenciesAreReady) {
    ResetManagerForTest();
    TaskGraph Graph;
    ResourceManager::Get().Init(Graph);

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

    Graph.Shutdown();
    ResourceManager::Get().Clear();
}

TEST(ResourceAccelerationStructureRequestTest, RendererScopedTlasRequestsDeduplicateAndReleaseAsTransient) {
    ResetManagerForTest();
    TaskGraph Graph;
    ResourceManager::Get().Init(Graph);

    const RHITopLevelAccelerationStructureDesc Desc{
        .InitialInstanceCapacity = 4,
        .BuildFlags = RHIAccelerationStructureBuildFlags::AllowUpdate,
    };
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

    Graph.Shutdown();
    ResourceManager::Get().Clear();
}

TEST(ResourceAccelerationStructureRequestTest, EmptyRendererScopePublishesFailure) {
    ResetManagerForTest();
    TaskGraph Graph;
    ResourceManager::Get().Init(Graph);

    auto Ref = ResourceManager::Get().RequestTopLevelAccelerationStructureRef(
        "", RHITopLevelAccelerationStructureDesc{.InitialInstanceCapacity = 1});
    ASSERT_TRUE(Ref);
    EXPECT_EQ(ResourceManager::Get().GetState(Ref.GetHandle()), ResourceState::Failed);

    Graph.Shutdown();
    ResourceManager::Get().Clear();
}

TEST(ResourceAccelerationStructureHardwareTest, DISABLED_MeshRequestResolvesSharedBlasPayload) {
    const auto* TestSourceDir = std::getenv("SOUL_ENGINE_TEST_SOURCE_DIR");
    ASSERT_NE(TestSourceDir, nullptr) << "Missing SOUL_ENGINE_TEST_SOURCE_DIR";

    const Path EngineDir = Path(TestSourceDir).parent_path().parent_path().parent_path().parent_path();
    ConfigManager::Get().Init(EngineDir);
    ASSERT_TRUE(ConfigManager::Get().LoadConfig().has_value());

    ASSERT_TRUE(glfwInit()) << "glfwInit failed";
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* Window = glfwCreateWindow(1, 1, "SoulEngine BLAS resource test", nullptr, nullptr);
    ASSERT_NE(Window, nullptr) << "glfwCreateWindow failed";

    auto DeviceResult = RHIRenderDevice::Create(Window);
    if (!DeviceResult) {
        glfwDestroyWindow(Window);
        glfwTerminate();
        GTEST_SKIP() << DeviceResult.error().ToString();
    }

    const auto Cleanup = [Window]() -> void {
        ResourceManager::Get().BeginShutdown();
        ResourceManager::Get().Clear();
        RHIRenderDevice::Destroy();
        glfwDestroyWindow(Window);
        glfwTerminate();
    };

    ResetManagerForTest();
    TaskGraph Graph;
    Graph.Init(1);
    ResourceManager::Get().Init(Graph);

    const Path MeshPath = EngineDir.parent_path() / "Applications" / "Test" / "Assets" / "teapot.obj";
    auto MeshRef = ResourceManager::Get().RequestMeshRef(MeshPath.string());
    ASSERT_TRUE(MeshRef);
    auto FirstBlasRef = ResourceManager::Get().RequestBottomLevelAccelerationStructureRef(MeshRef);
    auto SecondBlasRef = ResourceManager::Get().RequestBottomLevelAccelerationStructureRef(MeshRef);
    ASSERT_TRUE(FirstBlasRef);
    ASSERT_TRUE(SecondBlasRef);
    EXPECT_EQ(FirstBlasRef.GetHandle().GetKey(), SecondBlasRef.GetHandle().GetKey());

    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (IsPending(ResourceManager::Get().GetState(FirstBlasRef.GetHandle())) &&
           std::chrono::steady_clock::now() < Deadline) {
        for (std::size_t TaskIndex = 0; TaskIndex < TaskGraph::kMaxTasksPerPoll; ++TaskIndex) {
            auto Task = Graph.TryDequeue(ThreadQueue::RHI);
            if (!Task)
                break;
            (*Task)();
        }
        ResourceManager::Get().TickGpuPending();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_EQ(ResourceManager::Get().GetState(FirstBlasRef.GetHandle()), ResourceState::Ready);
    auto* BlasResource = ResourceManager::Get().TryGetReady(FirstBlasRef);
    ASSERT_NE(BlasResource, nullptr);
    EXPECT_NE(BlasResource->GetRhiPayload(), nullptr);

    FirstBlasRef.Reset();
    SecondBlasRef.Reset();
    MeshRef.Reset();
    Graph.Shutdown();
    Cleanup();
}

TEST(ResourceRefLifetimeTest, CachedAssetLastRefReleaseDoesNotMakeEntryStale) {
    ResetManagerForTest();
    TaskGraph Graph;
    ResourceManager::Get().Init(Graph);

    auto Ref    = ResourceManager::Get().RequestSampledTextureRef("CachedRefLifetimeTexture.png");
    auto Handle = Ref.GetHandle();
    ASSERT_TRUE(Handle.IsValid());
    EXPECT_EQ(ResourceManager::Get().GetState(Handle), ResourceState::CpuPreparing);

    Ref.Reset();

    EXPECT_EQ(ResourceManager::Get().GetState(Handle), ResourceState::CpuPreparing);

    Graph.Shutdown();
    ResourceManager::Get().Clear();
}

TEST(ResourceRefLifetimeTest, TransientLastRefReleaseMakesEntryStale) {
    ResetManagerForTest();
    TaskGraph Graph;
    ResourceManager::Get().Init(Graph);

    auto Ref    = ResourceManager::Get().RequestRenderTargetRef("TransientRefLifetimeDepth", MakeTestRenderTargetDesc());
    auto Handle = Ref.GetHandle();
    ASSERT_TRUE(Handle.IsValid());
    EXPECT_EQ(ResourceManager::Get().GetState(Handle), ResourceState::CpuPreparing);

    Ref.Reset();

    EXPECT_EQ(ResourceManager::Get().GetState(Handle), ResourceState::Stale);

    Graph.Shutdown();
    ResourceManager::Get().Clear();
}

TEST(ResourceRefLifetimeTest, MoveAssignmentReleasesPreviousTransientOwner) {
    ResetManagerForTest();
    TaskGraph Graph;
    ResourceManager::Get().Init(Graph);

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

    Graph.Shutdown();
    ResourceManager::Get().Clear();
}

TEST(ResourceRefLifetimeTest, CollectReleasedResourcesErasesReleasedTransientEntry) {
    ResetManagerForTest();
    TaskGraph Graph;
    ResourceManager::Get().Init(Graph);

    auto Ref    = ResourceManager::Get().RequestRenderTargetRef("TransientCollectReleased", MakeTestRenderTargetDesc());
    auto Handle = Ref.GetHandle();
    ASSERT_TRUE(Handle.IsValid());

    Ref.Reset();
    EXPECT_EQ(ResourceManager::Get().GetState(Handle), ResourceState::Stale);

    ResourceManager::Get().CollectReleasedResources();
    EXPECT_EQ(ResourceManager::Get().GetState(Handle), ResourceState::Unknown);

    Graph.Shutdown();
    ResourceManager::Get().Clear();
}

TEST(ResourceSampledTextureRequestTest, CoalescesNormalizedTexturePaths) {
    ResetManagerForTest();
    TaskGraph Graph;
    ResourceManager::Get().Init(Graph);

    auto ARef = ResourceManager::Get().RequestSampledTextureRef("Assets/../Textures/Missing.png");
    auto BRef = ResourceManager::Get().RequestSampledTextureRef("Textures/Missing.png");
    auto A    = ARef.GetHandle();
    auto B    = BRef.GetHandle();

    EXPECT_TRUE(A.IsValid());
    EXPECT_EQ(A.GetKey(), B.GetKey());
    EXPECT_EQ(A.GetGeneration(), B.GetGeneration());

    Graph.Shutdown();
    ResourceManager::Get().Clear();
}

TEST(ResourceSampledTextureRequestTest, RejectsRequestsAfterShutdownBegins) {
    ResetManagerForTest();
    TaskGraph Graph;
    ResourceManager::Get().Init(Graph);
    ResourceManager::Get().BeginShutdown();

    auto Ref    = ResourceManager::Get().RequestSampledTextureRef("Textures/Missing.png");
    auto Handle = Ref.GetHandle();

    EXPECT_FALSE(Handle.IsValid());

    Graph.Shutdown();
    ResourceManager::Get().Clear();
    ResourceManager::Get().Init(Graph);
}

TEST(ResourceSampledTextureRequestTest, MissingFilePublishesFailedResource) {
    ResetManagerForTest();
    TaskGraph Graph;
    Graph.Init(1);
    ResourceManager::Get().Init(Graph);

    auto Ref    = ResourceManager::Get().RequestSampledTextureRef("DefinitelyMissingTexture.png");
    auto Handle = Ref.GetHandle();

    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (IsPending(ResourceManager::Get().GetState(Handle)) && std::chrono::steady_clock::now() < Deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    EXPECT_EQ(ResourceManager::Get().GetState(Handle), ResourceState::Failed);
    ASSERT_TRUE(ResourceManager::Get().GetError(Handle).has_value());
    EXPECT_TRUE(ResourceManager::Get().GetError(Handle)->ToString().starts_with("stbi_load failed"));

    Graph.Shutdown();
    ResourceManager::Get().Clear();
}

TEST(ResourceSampledTextureRequestTest, StaleTexturePublishIgnored) {
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

TEST(ResourcePipelineRequestTest, CoalescesPipelineKeys) {
    ResetManagerForTest();
    TaskGraph Graph;
    ResourceManager::Get().Init(Graph);

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

    Graph.Shutdown();
    ResourceManager::Get().Clear();
}

TEST(ResourcePipelineRequestTest, ShaderCompileFailurePublishesFailedResource) {
    ResetManagerForTest();
    TaskGraph Graph;
    Graph.Init(1);
    ResourceManager::Get().Init(Graph);

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

    Graph.Shutdown();
    ResourceManager::Get().Clear();
}
