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
import Core;
import ShaderCompiler;

using namespace SoulEngine;

namespace {

/// Shared fixture: registry starts clean and the process-global TaskGraph is stopped before every test (tests
/// that need the lazy-start Pending recipe call TaskGraph::Get().Init(0) themselves). TearDown stops
/// TaskGraph and clears the registry so no Pending/Failed payloads outlive a test.
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

/// Graphics pass whose async pipeline never finishes in these tests: with the TaskGraph stopped, Ensure fails
/// outright (Failed); with zero workers the compile is queued but never runs (stays Pending).
struct AsyncGraphicsPass final : IRHIGraphicsPass {
    static constexpr StringView Name = "AsyncGraphics";
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    [[nodiscard]] static auto BuildPipelineRequest() -> GraphicsPipelineRequest {
        return {};
    }

    struct Parameter {
        RGTextureView<RHITextureUsage::RenderTarget, RHIAccess::Write> Output;
    };

    AsyncGraphicsPass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {}
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

/// Same recipe as a compute pass.
struct AsyncComputePass final : IRHIComputePass {
    static constexpr StringView Name = "AsyncCompute";
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    [[nodiscard]] static auto BuildPipelineRequest() -> ComputePipelineRequest {
        return {};
    }

    struct Parameter {
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Write> Output;
    };

    AsyncComputePass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {}
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

/// Graphics consumer: reads a storage buffer and writes an imported target (the side effect that anchors it
/// in the frame).
struct ConsumerPass final : IRHIGraphicsPass {
    static constexpr StringView Name = "Consumer";
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    struct Parameter {
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>         Input;
        RGTextureView<RHITextureUsage::RenderTarget, RHIAccess::Write> Output;
    };

    ConsumerPass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {}
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

} // namespace

TEST_F(PipelineRegistryTest, RegisterIsIdempotentAndUnknownKeysReportUnknown) {
    PipelineRegistry::Get().RegisterGraphics("GeometryPass", GraphicsPipelineRequest{});
    PipelineRegistry::Get().RegisterGraphics("GeometryPass", GraphicsPipelineRequest{}); // same key + desc: no-op

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
    // The fixture stopped the TaskGraph: EnsureRequestStarted cannot enqueue the async compile, so the entry
    // fails immediately and carries an error.
    PipelineRegistry::Get().RegisterGraphics("BrokenPass", GraphicsPipelineRequest{});
    EXPECT_EQ(PipelineRegistry::Get().GetState("BrokenPass"), RGPipelineState::Pending);

    PipelineRegistry::Get().EnsureRequestStarted("BrokenPass");
    EXPECT_EQ(PipelineRegistry::Get().GetState("BrokenPass"), RGPipelineState::Failed);
    EXPECT_TRUE(PipelineRegistry::Get().GetError("BrokenPass").has_value());
}

TEST_F(PipelineRegistryTest, LazyStartIsIdempotentWhilePendingWithZeroWorkers) {
    // TaskGraph running with zero workers: the async compile is queued but never consumed, so the entry stays
    // Pending. A second Ensure is a no-op.
    TaskGraph::Get().Init(0);

    PipelineRegistry::Get().RegisterCompute("LazyPass", ComputePipelineRequest{});
    PipelineRegistry::Get().EnsureRequestStarted("LazyPass");
    EXPECT_EQ(PipelineRegistry::Get().GetState("LazyPass"), RGPipelineState::Pending);

    PipelineRegistry::Get().EnsureRequestStarted("LazyPass"); // idempotent no-op
    EXPECT_EQ(PipelineRegistry::Get().GetState("LazyPass"), RGPipelineState::Pending);
    EXPECT_FALSE(PipelineRegistry::Get().GetError("LazyPass").has_value());
}

TEST_F(PipelineRegistryTest, PendingPipelineDropsTheSideEffectChainAsAnEmptyFrame) {
    // Zero workers: the pipeline stays Pending at Compile. The pass writes an imported target (a side
    // effect), so dropping it means the frame cannot produce its required output — the whole frame compiles
    // to an empty list.
    TaskGraph::Get().Init(0);
    PipelineRegistry::Get().Register<AsyncGraphicsPass>();

    RenderGraphBuilder Graph;
    const auto         Target = Graph.Import(RHIRef<RHITexture>{});
    Graph.AddPass<AsyncGraphicsPass>({.Output = Target});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_TRUE(List->Entries.empty());
}

TEST_F(PipelineRegistryTest, PendingPipelineCascadesToDependents) {
    // The compute producer is Pending; the graphics consumer depends on its output and is dropped with it.
    // The consumer carries the side effect, so the frame is empty — not an error, the soft-degrade contract.
    TaskGraph::Get().Init(0);
    PipelineRegistry::Get().Register<AsyncComputePass>();

    RenderGraphBuilder Graph;
    const auto         Scratch = Graph.CreateBuffer(
        "Scratch",
        RHIBufferDesc{.SizeBytes = 16, .Usage = RHIBufferUsage::Storage, .Lifetime = RHILifetime::Transient});
    const auto Target = Graph.Import(RHIRef<RHITexture>{});
    Graph.AddPass<AsyncComputePass>({.Output = Scratch});
    Graph.AddPass<ConsumerPass>({.Input = Scratch, .Output = Target});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_TRUE(List->Entries.empty());
}

TEST_F(PipelineRegistryTest, DeadPipelinePassIsPrunedBeforeAnyPipelineLookup) {
    // The pass writes a transient nobody reads: dead code, dropped by the side-effect prune before the
    // pipeline state is ever queried — so even its Failed pipeline cannot fail a frame that never needed the
    // pass.
    PipelineRegistry::Get().Register<AsyncGraphicsPass>();
    PipelineRegistry::Get().EnsureRequestStarted(PipelineKeyOf<AsyncGraphicsPass>()); // → Failed (TaskGraph stopped)
    ASSERT_EQ(PipelineRegistry::Get().GetState(PipelineKeyOf<AsyncGraphicsPass>()), RGPipelineState::Failed);

    RenderGraphBuilder Graph;
    const auto         Scratch = Graph.CreateTexture(
        "Scratch",
        RHITextureDesc{
            .Width = 8, .Height = 8, .Format = RHIFormat::B8G8R8A8_UNORM, .Usage = RHITextureUsage::RenderTarget});
    Graph.AddPass<AsyncGraphicsPass>({.Output = Scratch});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_TRUE(List->Entries.empty());
}

TEST_F(PipelineRegistryTest, FailedPipelineOnARequiredPassIsAFrameError) {
    // The fixture's stopped TaskGraph makes Ensure fail immediately. The pass writes an imported target, so
    // it cannot be pruned: the frame is a hard error and names the pass.
    PipelineRegistry::Get().Register<AsyncGraphicsPass>();

    RenderGraphBuilder Graph;
    const auto         Target = Graph.Import(RHIRef<RHITexture>{});
    Graph.AddPass<AsyncGraphicsPass>({.Output = Target});

    auto List = Graph.Compile();
    ASSERT_FALSE(List);
    EXPECT_NE(List.error().ToString().find("AsyncGraphics"), String::npos);
}
