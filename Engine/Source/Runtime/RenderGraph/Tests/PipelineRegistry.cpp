/// @file   PipelineRegistry.cpp
/// @brief  gtest coverage for the PipelineRegistry (type-hash keys +
///         Register<TPass>) and the graph's pipeline prune: Pending pipelines
///         drop their pass chain, Failed pipelines are hard frame errors.
///         All tests are device-free: they never publish a native RHI
///         payload, so no RHIRenderDevice is required (empty RHIRef payloads
///         release through the null-object early return).

#include <entt/entt.hpp>
#include <gtest/gtest.h>

import RenderGraph;
import RHI;
import TaskGraph;
import std;

using namespace SoulEngine;

namespace {

/// Shared fixture: registry starts clean and the process-global TaskGraph is
/// stopped before every test (tests that need the lazy-start Pending recipe
/// call TaskGraph::Get().Init(0) themselves). TearDown stops TaskGraph and
/// clears the registry so no Pending/Failed payloads outlive a test.
class PipelineRegistryTest : public testing::Test {
  protected:
    auto SetUp() -> void override {
        TaskGraph::Get().Shutdown();
        PipelineRegistry::Get().Init();
    }

    auto TearDown() -> void override {
        TaskGraph::Get().Shutdown();
        PipelineRegistry::Get().Clear();
    }
};

/// Graphics pass whose async pipeline never finishes in these tests: with the
/// TaskGraph stopped, Ensure fails outright (Failed); with zero workers the
/// compile is queued but never runs (stays Pending).
struct AsyncGraphicsPass final : IRHIGraphicsPass {
    static constexpr StringView Name = "AsyncGraphics";

    [[nodiscard]] static auto BuildPipelineRequest() -> GraphicsPipelineRequest {
        return {};
    }

    struct Parameter {
        RGColorRT Output = {};
    };

    AsyncGraphicsPass(Parameter In, RHIRef<RHIGraphicsPipeline> Pipeline)
        : IRHIGraphicsPass(String(Name), std::move(Pipeline)), m_Parameter(std::move(In)) {}
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override { return {}; }

  private:
    Parameter m_Parameter = {};
};

/// Same recipe as a compute pass.
struct AsyncComputePass final : IRHIComputePass {
    static constexpr StringView Name = "AsyncCompute";

    [[nodiscard]] static auto BuildPipelineRequest() -> ComputePipelineRequest {
        return {};
    }

    struct Parameter {
        RGStorageBufferUAV Output = {};
    };

    AsyncComputePass(Parameter In, RHIRef<RHIComputePipeline> Pipeline)
        : IRHIComputePass(String(Name), std::move(Pipeline)), m_Parameter(std::move(In)) {}
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override { return {}; }

  private:
    Parameter m_Parameter = {};
};

/// Graphics consumer: reads a storage buffer and writes an imported target
/// (the side effect that anchors it in the frame).
struct ConsumerPass final : IRHIGraphicsPass {
    static constexpr StringView Name = "Consumer";

    struct Parameter {
        RGStorageBufferSRV Input  = {};
        RGColorRT          Output = {};
    };

    ConsumerPass(Parameter In)
        : IRHIGraphicsPass(String(Name), nullptr), m_Parameter(std::move(In)) {}
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override { return {}; }

  private:
    Parameter m_Parameter = {};
};

} // namespace

TEST_F(PipelineRegistryTest, RegisterIsIdempotentAndUnknownKeysReportUnknown) {
    PipelineRegistry::Get().RegisterGraphics("GeometryPass", GraphicsPipelineRequest{});
    PipelineRegistry::Get().RegisterGraphics("GeometryPass", GraphicsPipelineRequest{});  // same key + desc: no-op

    EXPECT_TRUE(PipelineRegistry::Get().IsRegistered("GeometryPass"));
    EXPECT_FALSE(PipelineRegistry::Get().IsRegistered("NeverRegisteredPass"));
    EXPECT_EQ(PipelineRegistry::Get().GetState("NeverRegisteredPass"), RGPipelineState::Unknown);
}

TEST_F(PipelineRegistryTest, TypedRegisterKeysByPassType) {
    PipelineRegistry::Get().Register<AsyncGraphicsPass>();
    PipelineRegistry::Get().Register<AsyncComputePass>();

    EXPECT_TRUE(PipelineRegistry::Get().IsRegistered(PipelineKeyOf<AsyncGraphicsPass>()));
    EXPECT_TRUE(PipelineRegistry::Get().IsRegistered(PipelineKeyOf<AsyncComputePass>()));
}

TEST_F(PipelineRegistryTest, RegisteredButNotStartedMapsToPending) {
    PipelineRegistry::Get().RegisterCompute("CullingPass", ComputePipelineRequest{});
    EXPECT_EQ(PipelineRegistry::Get().GetState("CullingPass"), RGPipelineState::Pending);
    EXPECT_TRUE(PipelineRegistry::Get().IsRegistered("CullingPass"));
}

TEST_F(PipelineRegistryTest, EnqueueFailureWithStoppedTaskGraphMapsToFailed) {
    // The fixture stopped the TaskGraph: EnsureRequestStarted cannot enqueue
    // the async compile, so the entry fails immediately and carries an error.
    PipelineRegistry::Get().RegisterGraphics("BrokenPass", GraphicsPipelineRequest{});
    EXPECT_EQ(PipelineRegistry::Get().GetState("BrokenPass"), RGPipelineState::Pending);

    PipelineRegistry::Get().EnsureRequestStarted("BrokenPass");
    EXPECT_EQ(PipelineRegistry::Get().GetState("BrokenPass"), RGPipelineState::Failed);
    EXPECT_TRUE(PipelineRegistry::Get().GetError("BrokenPass").has_value());
}

TEST_F(PipelineRegistryTest, LazyStartIsIdempotentWhilePendingWithZeroWorkers) {
    // TaskGraph running with zero workers: the async compile is queued but
    // never consumed, so the entry stays Pending. A second Ensure is a no-op.
    TaskGraph::Get().Init(0);

    PipelineRegistry::Get().RegisterCompute("LazyPass", ComputePipelineRequest{});
    PipelineRegistry::Get().EnsureRequestStarted("LazyPass");
    EXPECT_EQ(PipelineRegistry::Get().GetState("LazyPass"), RGPipelineState::Pending);

    PipelineRegistry::Get().EnsureRequestStarted("LazyPass");  // idempotent no-op
    EXPECT_EQ(PipelineRegistry::Get().GetState("LazyPass"), RGPipelineState::Pending);
    EXPECT_FALSE(PipelineRegistry::Get().GetError("LazyPass").has_value());
}

TEST_F(PipelineRegistryTest, PendingPipelineDropsTheSideEffectChainAsAnEmptyFrame) {
    // Zero workers: the pipeline stays Pending at Compile. The pass writes an
    // imported target (a side effect), so dropping it means the frame cannot
    // produce its required output — the whole frame compiles to an empty list.
    TaskGraph::Get().Init(0);
    PipelineRegistry::Get().Register<AsyncGraphicsPass>();

    RenderGraph Graph;
    const auto  Target = Graph.Import(RHIRef<RHIRenderTarget>{});
    Graph.AddPass<AsyncGraphicsPass>({.Output = RGColorRT{.Texture = Target}});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_TRUE(List->Passes.empty());
}

TEST_F(PipelineRegistryTest, PendingPipelineCascadesToDependents) {
    // The compute producer is Pending; the graphics consumer depends on its
    // output and is dropped with it. The consumer carries the side effect, so
    // the frame is empty — not an error, the soft-degrade contract.
    TaskGraph::Get().Init(0);
    PipelineRegistry::Get().Register<AsyncComputePass>();

    RenderGraph Graph;
    const auto  Scratch = Graph.CreateShaderStorageBuffer(
        "Scratch", RGShaderStorageBufferDesc{.SizeBytes = 16, .Usage = RHITransientBufferUsage::ShaderRead});
    const auto Target = Graph.Import(RHIRef<RHIRenderTarget>{});
    Graph.AddPass<AsyncComputePass>({.Output = RGStorageBufferUAV{.Buffer = Scratch}});
    Graph.AddPass<ConsumerPass>({.Input  = RGStorageBufferSRV{.Buffer = Scratch},
                                 .Output = RGColorRT{.Texture = Target}});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_TRUE(List->Passes.empty());
}

TEST_F(PipelineRegistryTest, DeadPipelinePassIsPrunedBeforeAnyPipelineLookup) {
    // The pass writes a transient nobody reads: dead code, dropped by the
    // side-effect prune before the pipeline state is ever queried — so even
    // its Failed pipeline cannot fail a frame that never needed the pass.
    PipelineRegistry::Get().Register<AsyncGraphicsPass>();
    PipelineRegistry::Get().EnsureRequestStarted(PipelineKeyOf<AsyncGraphicsPass>());  // → Failed (TaskGraph stopped)
    ASSERT_EQ(PipelineRegistry::Get().GetState(PipelineKeyOf<AsyncGraphicsPass>()), RGPipelineState::Failed);

    RenderGraph Graph;
    const auto  Scratch = Graph.CreateTexture(
        "Scratch", RGTextureDesc{.Width = 8, .Height = 8, .Format = RHIFormat::B8G8R8A8_UNORM});
    Graph.AddPass<AsyncGraphicsPass>({.Output = RGColorRT{.Texture = Scratch}});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_TRUE(List->Passes.empty());
}

TEST_F(PipelineRegistryTest, FailedPipelineOnARequiredPassIsAFrameError) {
    // The fixture's stopped TaskGraph makes Ensure fail immediately. The pass
    // writes an imported target, so it cannot be pruned: the frame is a hard
    // error and names the pass.
    PipelineRegistry::Get().Register<AsyncGraphicsPass>();

    RenderGraph Graph;
    const auto  Target = Graph.Import(RHIRef<RHIRenderTarget>{});
    Graph.AddPass<AsyncGraphicsPass>({.Output = RGColorRT{.Texture = Target}});

    auto List = Graph.Compile();
    ASSERT_FALSE(List);
    EXPECT_NE(List.error().ToString().find("AsyncGraphics"), String::npos);
}
