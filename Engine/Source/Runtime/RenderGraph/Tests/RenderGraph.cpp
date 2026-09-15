/// @file   RenderGraph.cpp
/// @brief  gtest coverage for the RenderGraph Parameter-API orchestration:
///         last-writer ordering, side-effect pruning, pipeline pruning,
///         transient realization, and the single-call Compile contract.
///
/// Mock passes are always typed: a local struct with `Name`, a nested
/// `Parameter` whose view-typed fields ARE the resource declaration, and a
/// constructor taking the Parameter (pipeline passes additionally take the
/// Compile-stashed pipeline — the pipeline-side tests live in
/// PipelineRegistry.cpp). Non-pipeline mocks omit `BuildPipelineRequest`, so
/// they never touch the PipelineRegistry or the TaskGraph.

#include <entt/entt.hpp>
#include <gtest/gtest.h>

import RenderGraph;
import RHI;
import WindowSystem;
import std;

using namespace SoulEngine;

namespace {

// ── Test device ────────────────────────────────────────────────────────────
// Inherits the base RHIRenderDevice defaults (every other method logs "not
// implemented" and fails) except the three transient-create calls the graph's
// realize step makes — those succeed with an empty RHIRef, so Compile can
// walk a graph full of transients without a GPU. Each call is counted so
// tests can assert what was and was not realized.

class MockRenderDevice final : public RHIRenderDevice {
  public:
    Uint32 CreatedTargets         = 0;
    Uint32 CreatedStorageBuffers  = 0;
    Uint32 CreatedConstantBuffers = 0;
    /// Usage bits of the last realized render target — lets tests assert the
    /// graph's derived usage (ADR 05).
    RHITextureUsage LastTargetUsage = RHITextureUsage::None;
    /// Bytes the last transient upload carried — lets tests assert what
    /// realize actually forwarded to the RHI.
    std::vector<std::byte> LastUpload = {};

    [[nodiscard]] auto Initialize(IWindowSystem*) -> std::expected<void, ErrorMessage> override {
        return {};
    }
    [[nodiscard]] auto CreateRenderTarget(StringView, const RHIRenderTargetDesc& Desc)
        -> std::expected<RHIRef<RHIRenderTarget>, ErrorMessage> override {
        ++CreatedTargets;
        LastTargetUsage = Desc.Usage;
        return RHIRef<RHIRenderTarget>{};
    }
    [[nodiscard]] auto CreateTransientShaderStorageBuffer(StringView,
                                                          const RHITransientShaderStorageBufferDesc& Desc)
        -> std::expected<RHIRef<RHITransientShaderStorageBuffer>, ErrorMessage> override {
        ++CreatedStorageBuffers;
        if (Desc.InitialData.has_value())
            LastUpload.assign(Desc.InitialData->begin(), Desc.InitialData->end());
        return RHIRef<RHITransientShaderStorageBuffer>{};
    }
    [[nodiscard]] auto CreateTransientConstantBuffer(StringView, const RHITransientConstantBufferDesc&)
        -> std::expected<RHIRef<RHITransientConstantBuffer>, ErrorMessage> override {
        ++CreatedConstantBuffers;
        return RHIRef<RHITransientConstantBuffer>{};
    }
};

/// Minimal window system: satisfies the non-null check in
/// RHIRenderDevice::Create; the mock device never touches it.
class MockWindowSystem final : public IWindowSystem {
  public:
    [[nodiscard]] auto GetType() const -> WindowSystemType override { return WindowSystemType::Unknown; }
    [[nodiscard]] auto IsValid() const -> bool override { return true; }
    auto Shutdown() -> void override {}
    [[nodiscard]] auto Tick() -> bool override { return true; }
    [[nodiscard]] auto GetEventDispatcher() -> entt::dispatcher& override { return m_Dispatcher; }
    [[nodiscard]] auto GetFramebufferExtent() const -> FramebufferExtent override { return {}; }
    auto SetCursorMode(CursorMode) -> void override {}

  private:
    entt::dispatcher m_Dispatcher{};
};

/// Registers the mock backend and creates it through the normal
/// Create()/factory path before main() runs, so every test in this binary
/// sees the mock device through RHIRenderDevice::Get(). The device is
/// intentionally leaked: at static-destruction time the logger and
/// allocators may already be gone, and the OS reclaims the memory either
/// way.
struct DeviceInjector {
    DeviceInjector() {
        RHIBackendFactory::Get().Register<MockRenderDevice>("Test");
        static auto Window = MockWindowSystem{};
        const auto Result = RHIRenderDevice::Create("Test", &Window);
        (void)Result;
    }
    [[nodiscard]] static auto Get() -> MockRenderDevice& {
        return static_cast<MockRenderDevice&>(RHIRenderDevice::Get());
    }
};
const auto G_InjectDevice = DeviceInjector{};

// ── Mock passes ────────────────────────────────────────────────────────────
// Each mock counts its constructions; tests assert both presence in the
// compiled list (via GetName) and construction counts (a pruned pass must
// never be constructed).

struct WriteColorPass final : IRHIGraphicsPass {
    static constexpr StringView Name = "WriteColor";
    static inline Uint32        Constructed = 0;

    struct Parameter {
        RGColorRT Output = {};
    };

    WriteColorPass(Parameter In)
        : IRHIGraphicsPass(String(Name), nullptr), m_Parameter(std::move(In)) {
        ++Constructed;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override { return {}; }

  private:
    Parameter m_Parameter = {};
};

struct ReadColorPass final : IRHIGraphicsPass {
    static constexpr StringView Name = "ReadColor";
    static inline Uint32        Constructed = 0;

    struct Parameter {
        RGTextureSRV Input = {};
    };

    ReadColorPass(Parameter In)
        : IRHIGraphicsPass(String(Name), nullptr), m_Parameter(std::move(In)) {
        ++Constructed;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override { return {}; }

  private:
    Parameter m_Parameter = {};
};

struct WriteStoragePass final : IRHITransferPass {
    static constexpr StringView Name = "WriteStorage";
    static inline Uint32        Constructed = 0;

    struct Parameter {
        RGStorageBufferUAV Output = {};
    };

    WriteStoragePass(Parameter In) : IRHITransferPass(String(Name)), m_Parameter(std::move(In)) {
        ++Constructed;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override { return {}; }

  private:
    Parameter m_Parameter = {};
};

struct ReadStoragePass final : IRHITransferPass {
    static constexpr StringView Name = "ReadStorage";
    static inline Uint32        Constructed = 0;

    struct Parameter {
        RGStorageBufferSRV Input = {};
    };

    ReadStoragePass(Parameter In) : IRHITransferPass(String(Name)), m_Parameter(std::move(In)) {
        ++Constructed;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override { return {}; }

  private:
    Parameter m_Parameter = {};
};

/// NeverPrune variant: pruning must keep this pass even with no downstream
/// consumers.
struct KeepReadStoragePass final : IRHITransferPass {
    static constexpr StringView Name       = "KeepReadStorage";
    static constexpr bool       NeverPrune = true;
    static inline Uint32        Constructed = 0;

    struct Parameter {
        RGStorageBufferSRV Input = {};
    };

    KeepReadStoragePass(Parameter In) : IRHITransferPass(String(Name)), m_Parameter(std::move(In)) {
        ++Constructed;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override { return {}; }

  private:
    Parameter m_Parameter = {};
};

/// Compute pass mixing views and passthrough CPU scalars: the aggregate walk
/// must declare access for the views only and leave the scalars untouched.
struct FilterPass final : IRHIComputePass {
    static constexpr StringView Name = "Filter";
    static inline Uint32        Constructed = 0;
    static inline Uint32        LastTag     = 0;

    struct Parameter {
        RGStorageBufferSRV Input  = {};
        RGStorageBufferUAV Output = {};
        Uint32             Tag    = 0;
        Float32            Scale  = 1.0f;
    };

    FilterPass(Parameter In) : IRHIComputePass(String(Name)), m_Parameter(std::move(In)) {
        ++Constructed;
        LastTag = m_Parameter.Tag;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override { return {}; }

  private:
    Parameter m_Parameter = {};
};

struct KeepFilterPass final : IRHIComputePass {
    static constexpr StringView Name       = "KeepFilter";
    static constexpr bool       NeverPrune = true;
    static inline Uint32        Constructed = 0;

    struct Parameter {
        RGStorageBufferSRV Input  = {};
        RGStorageBufferUAV Output = {};
    };

    KeepFilterPass(Parameter In) : IRHIComputePass(String(Name)), m_Parameter(std::move(In)) {
        ++Constructed;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override { return {}; }

  private:
    Parameter m_Parameter = {};
};

struct ReadConstantPass final : IRHIComputePass {
    static constexpr StringView Name       = "ReadConstant";
    static constexpr bool       NeverPrune = true;
    static inline Uint32        Constructed = 0;

    struct Parameter {
        RGConstantBufferSRV Input = {};
    };

    ReadConstantPass(Parameter In) : IRHIComputePass(String(Name)), m_Parameter(std::move(In)) {
        ++Constructed;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override { return {}; }

  private:
    Parameter m_Parameter = {};
};

struct CopyPass final : IRHITransferPass {
    static constexpr StringView Name = "Copy";
    static inline Uint32        Constructed = 0;

    struct Parameter {
        RGCopySrc Source = {};
        RGCopyDst Target = {};
    };

    CopyPass(Parameter In) : IRHITransferPass(String(Name)), m_Parameter(std::move(In)) {
        ++Constructed;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override { return {}; }

  private:
    Parameter m_Parameter = {};
};

/// Graphics pass mixing a storage read with an imported color write: the
/// imported write is the side effect that anchors its producer chain.
struct BridgePass final : IRHIGraphicsPass {
    static constexpr StringView Name = "Bridge";

    struct Parameter {
        RGStorageBufferSRV Input  = {};
        RGColorRT          Output = {};
    };

    BridgePass(Parameter In)
        : IRHIGraphicsPass(String(Name), nullptr), m_Parameter(std::move(In)) {}
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override { return {}; }

  private:
    Parameter m_Parameter = {};
};

/// Depth-attachment writer mock: contributes DepthStencil to the texture's
/// derived usage.
struct WriteDepthPass final : IRHIGraphicsPass {
    static constexpr StringView Name        = "WriteDepth";
    static inline Uint32        Constructed = 0;

    struct Parameter {
        RGDepthRT Depth = {};
    };

    WriteDepthPass(Parameter In)
        : IRHIGraphicsPass(String(Name), nullptr), m_Parameter(std::move(In)) {
        ++Constructed;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override { return {}; }

  private:
    Parameter m_Parameter = {};
};

/// Storage-image writer mock: contributes ShaderStorage to the texture's
/// derived usage while it survives pruning.
struct WriteStorageTexturePass final : IRHIComputePass {
    static constexpr StringView Name        = "WriteStorageTexture";
    static inline Uint32        Constructed = 0;

    struct Parameter {
        RGStorageTextureUAV Output = {};
    };

    WriteStorageTexturePass(Parameter In)
        : IRHIComputePass(String(Name)), m_Parameter(std::move(In)) {
        ++Constructed;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override { return {}; }

  private:
    Parameter m_Parameter = {};
};

/// NeverPrune storage reader used as the frame anchor in ordering tests.
struct AnchorPass final : IRHITransferPass {
    static constexpr StringView Name       = "Anchor";
    static constexpr bool       NeverPrune = true;

    struct Parameter {
        RGStorageBufferSRV Input = {};
    };

    AnchorPass(Parameter In) : IRHITransferPass(String(Name)), m_Parameter(std::move(In)) {}
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override { return {}; }

  private:
    Parameter m_Parameter = {};
};

auto ResetMockCounters() -> void {
    WriteColorPass::Constructed      = 0;
    WriteDepthPass::Constructed      = 0;
    WriteStorageTexturePass::Constructed = 0;
    ReadColorPass::Constructed       = 0;
    WriteStoragePass::Constructed    = 0;
    ReadStoragePass::Constructed     = 0;
    KeepReadStoragePass::Constructed = 0;
    FilterPass::Constructed          = 0;
    KeepFilterPass::Constructed      = 0;
    ReadConstantPass::Constructed    = 0;
    CopyPass::Constructed            = 0;
    DeviceInjector::Get().CreatedTargets         = 0;
    DeviceInjector::Get().CreatedStorageBuffers  = 0;
    DeviceInjector::Get().CreatedConstantBuffers = 0;
    DeviceInjector::Get().LastUpload.clear();
    // Cross-frame pool state must not leak between tests.
    RenderGraph::Get().GetPool().Clear();
}

/// Pass names in compiled order — the observable result of ordering + pruning.
[[nodiscard]] auto PassNames(const RenderPassList& List) -> std::vector<StringView> {
    std::vector<StringView> Names;
    Names.reserve(List.Passes.size());
    for (const auto& Pass : List.Passes)
        Names.push_back(Pass->GetName());
    return Names;
}

[[nodiscard]] auto MakeStorage(RenderGraphBuilder& Graph, StringView Name, Uint64 SizeBytes = 16) -> RGStorageBufferHandle {
    return Graph.CreateShaderStorageBuffer(
        Name, RGShaderStorageBufferDesc{.SizeBytes = SizeBytes, .Usage = RHITransientBufferUsage::ShaderRead});
}

} // namespace

// ── Ordering ───────────────────────────────────────────────────────────────

TEST(RenderGraphOrder, IndependentPassesKeepRegistrationOrder) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto  X = MakeStorage(Graph, "X");
    const auto  Y = MakeStorage(Graph, "Y");
    Graph.AddPass<WriteStoragePass>({.Output = RGStorageBufferUAV{.Buffer = X}});
    Graph.AddPass<WriteStoragePass>({.Output = RGStorageBufferUAV{.Buffer = Y}});
    // A NeverPrune reader anchors both producers in the frame.
    Graph.AddPass<KeepFilterPass>({.Input  = RGStorageBufferSRV{.Buffer = X},
                                   .Output = RGStorageBufferUAV{.Buffer = Y}});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(PassNames(*List), (std::vector<StringView>{"WriteStorage", "WriteStorage", "KeepFilter"}));
}

TEST(RenderGraphOrder, ReaderOrdersAfterLastWriterOnly) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto  X = MakeStorage(Graph, "X");
    const auto  Y = MakeStorage(Graph, "Y");
    Graph.AddPass<WriteStoragePass>({.Output = RGStorageBufferUAV{.Buffer = X}});   // X writer
    Graph.AddPass<FilterPass>({.Input  = RGStorageBufferSRV{.Buffer = X},           // reads X, writes Y
                               .Output = RGStorageBufferUAV{.Buffer = Y}});
    Graph.AddPass<KeepFilterPass>({.Input  = RGStorageBufferSRV{.Buffer = Y},       // depends on Filter only
                                   .Output = RGStorageBufferUAV{.Buffer = X}});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(PassNames(*List), (std::vector<StringView>{"WriteStorage", "Filter", "KeepFilter"}));
}

TEST(RenderGraphOrder, MultipleWritersChainInRegistrationOrder) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto  X = MakeStorage(Graph, "X");
    Graph.AddPass<WriteStoragePass>({.Output = RGStorageBufferUAV{.Buffer = X}});
    Graph.AddPass<FilterPass>({.Input  = RGStorageBufferSRV{.Buffer = X},
                               .Output = RGStorageBufferUAV{.Buffer = X}});
    Graph.AddPass<KeepReadStoragePass>({.Input = RGStorageBufferSRV{.Buffer = X}});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(PassNames(*List), (std::vector<StringView>{"WriteStorage", "Filter", "KeepReadStorage"}));
}

// ── Pruning ────────────────────────────────────────────────────────────────

TEST(RenderGraphPrune, DeadProducerIsNeverConstructed) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto  Dead = MakeStorage(Graph, "Dead");
    const auto  Live = MakeStorage(Graph, "Live");
    Graph.AddPass<WriteStoragePass>({.Output = RGStorageBufferUAV{.Buffer = Dead}});  // nobody reads Dead
    Graph.AddPass<KeepReadStoragePass>({.Input = RGStorageBufferSRV{.Buffer = Live}});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(PassNames(*List), (std::vector<StringView>{"KeepReadStorage"}));
    EXPECT_EQ(WriteStoragePass::Constructed, 0u);
}

TEST(RenderGraphPrune, PruneIsTransitiveOverTheDependencyChain) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto  X = MakeStorage(Graph, "X");
    const auto  Y = MakeStorage(Graph, "Y");
    Graph.AddPass<WriteStoragePass>({.Output = RGStorageBufferUAV{.Buffer = X}});
    Graph.AddPass<FilterPass>({.Input  = RGStorageBufferSRV{.Buffer = X},
                               .Output = RGStorageBufferUAV{.Buffer = Y}});
    Graph.AddPass<ReadStoragePass>({.Input = RGStorageBufferSRV{.Buffer = Y}});  // no side effect
    // Independent side-effect pass keeps the frame non-empty.
    const auto Imported = Graph.Import(RHIRef<RHIRenderTarget>{});
    Graph.AddPass<WriteColorPass>({.Output = RGColorRT{.Texture = Imported}});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(PassNames(*List), (std::vector<StringView>{"WriteColor"}));
    EXPECT_EQ(WriteStoragePass::Constructed, 0u);
    EXPECT_EQ(FilterPass::Constructed, 0u);
    EXPECT_EQ(ReadStoragePass::Constructed, 0u);
}

TEST(RenderGraphPrune, ImportedWriteKeepsItsProducerChainAlive) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto  X        = MakeStorage(Graph, "X");
    const auto  Imported = Graph.Import(RHIRef<RHIRenderTarget>{});
    Graph.AddPass<WriteStoragePass>({.Output = RGStorageBufferUAV{.Buffer = X}});
    Graph.AddPass<FilterPass>({.Input  = RGStorageBufferSRV{.Buffer = X},
                               .Output = RGStorageBufferUAV{.Buffer = X}});
    // Reads X and writes an imported target: the write is visible outside the
    // graph, so the whole chain survives.
    Graph.AddPass<BridgePass>({.Input  = RGStorageBufferSRV{.Buffer = X},
                               .Output = RGColorRT{.Texture = Imported}});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(PassNames(*List), (std::vector<StringView>{"WriteStorage", "Filter", "Bridge"}));
    EXPECT_EQ(DeviceInjector::Get().CreatedStorageBuffers, 1u);
}

// ── Blit to swapchain (explicit present, ADR 05) ──────────────────────────

TEST(RenderGraphBlitToSwapchain, NeverPruneSurvivesWithoutConsumers) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto  Target = Graph.Import(RHIRef<RHIRenderTarget>{});
    Graph.AddPass<BlitToSwapchainPass>(
        {.Source = RGCopySrc{.Texture = Target}, .DstWidth = 8, .DstHeight = 8});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    ASSERT_EQ(List->Passes.size(), 1u);
    EXPECT_EQ(List->Passes.front()->GetName(), "BlitToSwapchainPass");
}

TEST(RenderGraphBlitToSwapchain, OrdersAfterTheSourceWriter) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto  Target = Graph.CreateTexture(
        "SceneColor", {.Width = 8, .Height = 8, .Format = RHIFormat::B8G8R8A8_UNORM});
    Graph.AddPass<WriteColorPass>({.Output = RGColorRT{.Texture = Target}});
    Graph.AddPass<BlitToSwapchainPass>(
        {.Source = RGCopySrc{.Texture = Target}, .DstWidth = 8, .DstHeight = 8});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(PassNames(*List), (std::vector<StringView>{"WriteColor", "BlitToSwapchainPass"}));
}

// ── Derived usage (ADR 05) ─────────────────────────────────────────────────

TEST(RenderGraphDerivedUsage, SurvivingViewsFormTheUsageUnion) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto  Target = Graph.CreateTexture(
        "SceneColor", {.Width = 8, .Height = 8, .Format = RHIFormat::B8G8R8A8_UNORM});
    Graph.AddPass<WriteColorPass>({.Output = RGColorRT{.Texture = Target}});
    Graph.AddPass<BlitToSwapchainPass>(
        {.Source = RGCopySrc{.Texture = Target}, .DstWidth = 8, .DstHeight = 8});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    ASSERT_EQ(DeviceInjector::Get().CreatedTargets, 1u);
    const auto Usage = static_cast<Uint32>(DeviceInjector::Get().LastTargetUsage);
    EXPECT_NE(Usage & static_cast<Uint32>(RHITextureUsage::RenderTarget), 0u);
    EXPECT_NE(Usage & static_cast<Uint32>(RHITextureUsage::TransferSrc), 0u);
    EXPECT_EQ(Usage & static_cast<Uint32>(RHITextureUsage::ShaderStorage), 0u);
}

TEST(RenderGraphDerivedUsage, PrunedPassesContributeNoUsage) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto  Target = Graph.CreateTexture(
        "SceneColor", {.Width = 8, .Height = 8, .Format = RHIFormat::B8G8R8A8_UNORM});
    Graph.AddPass<WriteColorPass>({.Output = RGColorRT{.Texture = Target}});
    // No side effect and nothing required depends on it: this sampled read is
    // dead code and must not add ShaderResource to the derived usage.
    Graph.AddPass<ReadColorPass>({.Input = RGTextureSRV{.Texture = Target}});
    Graph.AddPass<BlitToSwapchainPass>(
        {.Source = RGCopySrc{.Texture = Target}, .DstWidth = 8, .DstHeight = 8});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(ReadColorPass::Constructed, 0u);
    ASSERT_EQ(DeviceInjector::Get().CreatedTargets, 1u);
    const auto Usage = static_cast<Uint32>(DeviceInjector::Get().LastTargetUsage);
    EXPECT_EQ(Usage & static_cast<Uint32>(RHITextureUsage::ShaderResource), 0u);
}

// ── Render-target pool (ADR 05) ─────────────────────────────────────────────

TEST(RenderGraphPool, ReusesAcrossBuilderLifetimes) {
    ResetMockCounters();
    const auto MakeFrame = [] {
        RenderGraphBuilder Graph;
        const auto Target = Graph.CreateTexture(
            "SceneColor", {.Width = 8, .Height = 8, .Format = RHIFormat::B8G8R8A8_UNORM});
        Graph.AddPass<WriteColorPass>({.Output = RGColorRT{.Texture = Target}});
        Graph.AddPass<BlitToSwapchainPass>(
            {.Source = RGCopySrc{.Texture = Target}, .DstWidth = 8, .DstHeight = 8});
        auto List = Graph.Compile();
        EXPECT_TRUE(List);
        // Builder dies here: pooled refs return immediately (no GPU gate).
    };
    MakeFrame();
    MakeFrame();
    // Second frame's identical desc must hit the pool: no new creation.
    EXPECT_EQ(DeviceInjector::Get().CreatedTargets, 1u);
}

TEST(RenderGraphPool, KeyDistinguishesDerivedUsage) {
    ResetMockCounters();
    const auto MakeFrame = [](bool Storage) {
        RenderGraphBuilder Graph;
        const auto Target = Graph.CreateTexture(
            "Shared", {.Width = 8, .Height = 8, .Format = RHIFormat::B8G8R8A8_UNORM});
        if (Storage)
            Graph.AddPass<WriteStorageTexturePass>({.Output = RGStorageTextureUAV{.Texture = Target}});
        else
            Graph.AddPass<WriteColorPass>({.Output = RGColorRT{.Texture = Target}});
        Graph.AddPass<BlitToSwapchainPass>(
            {.Source = RGCopySrc{.Texture = Target}, .DstWidth = 8, .DstHeight = 8});
        auto List = Graph.Compile();
        EXPECT_TRUE(List);
    };
    MakeFrame(false);  // RenderTarget | TransferSrc
    MakeFrame(true);   // ShaderStorage | TransferSrc — different bucket
    EXPECT_EQ(DeviceInjector::Get().CreatedTargets, 2u);
}

TEST(RenderGraphPool, TickEvictsIdleEntries) {
    ResetMockCounters();
    {
        RenderGraphBuilder Graph;
        const auto Target = Graph.CreateTexture(
            "SceneColor", {.Width = 8, .Height = 8, .Format = RHIFormat::B8G8R8A8_UNORM});
        Graph.AddPass<WriteColorPass>({.Output = RGColorRT{.Texture = Target}});
        Graph.AddPass<BlitToSwapchainPass>(
            {.Source = RGCopySrc{.Texture = Target}, .DstWidth = 8, .DstHeight = 8});
        auto List = Graph.Compile();
        ASSERT_TRUE(List);
    }
    ASSERT_EQ(RenderGraph::Get().GetPool().GetFreeEntryCount(), 1u);
    for (Uint32 I = 0; I < kRGEvictIdleFrames; ++I)
        RenderGraph::Get().Tick();
    EXPECT_EQ(RenderGraph::Get().GetPool().GetFreeEntryCount(), 0u);
    // After eviction the same desc is a miss again.
    {
        RenderGraphBuilder Graph;
        const auto Target = Graph.CreateTexture(
            "SceneColor", {.Width = 8, .Height = 8, .Format = RHIFormat::B8G8R8A8_UNORM});
        Graph.AddPass<WriteColorPass>({.Output = RGColorRT{.Texture = Target}});
        Graph.AddPass<BlitToSwapchainPass>(
            {.Source = RGCopySrc{.Texture = Target}, .DstWidth = 8, .DstHeight = 8});
        auto List = Graph.Compile();
        ASSERT_TRUE(List);
    }
    EXPECT_EQ(DeviceInjector::Get().CreatedTargets, 2u);
}

TEST(RenderGraphDerivedUsage, DepthAttachmentDerivesDepthStencil) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto         Target = Graph.CreateTexture(
        "Depth", {.Width = 8, .Height = 8, .Format = RHIFormat::D32_SFLOAT});
    Graph.AddPass<WriteDepthPass>({.Depth = RGDepthRT{.Texture = Target}});
    // A depth write alone is dead code; the NeverPrune reader anchors it.
    Graph.AddPass<BlitToSwapchainPass>(
        {.Source = RGCopySrc{.Texture = Target}, .DstWidth = 8, .DstHeight = 8});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    ASSERT_EQ(DeviceInjector::Get().CreatedTargets, 1u);
    const auto Usage = static_cast<Uint32>(DeviceInjector::Get().LastTargetUsage);
    EXPECT_NE(Usage & static_cast<Uint32>(RHITextureUsage::DepthStencil), 0u);
    EXPECT_EQ(Usage & static_cast<Uint32>(RHITextureUsage::RenderTarget), 0u);
}

TEST(RenderGraphDerivedUsage, StorageImageWriteDerivesShaderStorage) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto  Target = Graph.CreateTexture(
        "RT/Output", {.Width = 8, .Height = 8, .Format = RHIFormat::B8G8R8A8_UNORM});
    Graph.AddPass<WriteStorageTexturePass>({.Output = RGStorageTextureUAV{.Texture = Target}});
    Graph.AddPass<BlitToSwapchainPass>(
        {.Source = RGCopySrc{.Texture = Target}, .DstWidth = 8, .DstHeight = 8});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    ASSERT_EQ(DeviceInjector::Get().CreatedTargets, 1u);
    const auto Usage = static_cast<Uint32>(DeviceInjector::Get().LastTargetUsage);
    EXPECT_NE(Usage & static_cast<Uint32>(RHITextureUsage::ShaderStorage), 0u);
    EXPECT_NE(Usage & static_cast<Uint32>(RHITextureUsage::TransferSrc), 0u);
    EXPECT_EQ(Usage & static_cast<Uint32>(RHITextureUsage::RenderTarget), 0u);
}

// ── Realization ────────────────────────────────────────────────────────────

TEST(RenderGraphRealize, DeadTransientsAreNeverRealized) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto  Dead = MakeStorage(Graph, "Dead");
    const auto  Live = MakeStorage(Graph, "Live");
    Graph.AddPass<WriteStoragePass>({.Output = RGStorageBufferUAV{.Buffer = Dead}});
    Graph.AddPass<KeepReadStoragePass>({.Input = RGStorageBufferSRV{.Buffer = Live}});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(DeviceInjector::Get().CreatedStorageBuffers, 1u);
}

TEST(RenderGraphRealize, TransientWithInitialDataIsReadableWithoutAWriter) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    static constexpr std::array<std::byte, 16> Payload{};
    const auto Buffer = Graph.CreateShaderStorageBuffer(
        "UploadTable",
        RGShaderStorageBufferDesc{
            .SizeBytes   = Payload.size(),
            .Usage       = RHITransientBufferUsage::ShaderRead,
            .InitialData = std::span<const std::byte>{Payload},
        });
    Graph.AddPass<KeepReadStoragePass>({.Input = RGStorageBufferSRV{.Buffer = Buffer}});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(PassNames(*List), (std::vector<StringView>{"KeepReadStorage"}));
    EXPECT_EQ(DeviceInjector::Get().CreatedStorageBuffers, 1u);
}

TEST(RenderGraphRealize, TransientWithoutInitialDataIsAlsoReadableWithoutAWriter) {
    // No InitialData means undefined scratch contents; the graph does not
    // police that — the reader simply reads whatever the GPU left there.
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto  Scratch = MakeStorage(Graph, "Scratch");
    Graph.AddPass<KeepReadStoragePass>({.Input = RGStorageBufferSRV{.Buffer = Scratch}});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(DeviceInjector::Get().CreatedStorageBuffers, 1u);
}

TEST(RenderGraphRealize, ConstantBufferWithInitialDataRealizesOnce) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    static constexpr std::array<std::byte, 32> Payload{};
    const auto CB = Graph.CreateConstantBuffer(
        "FrameConstants",
        RGConstantBufferDesc{.SizeBytes   = Payload.size(),
                             .InitialData = std::span<const std::byte>{Payload}});
    Graph.AddPass<ReadConstantPass>({.Input = {.Buffer = CB}});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(DeviceInjector::Get().CreatedConstantBuffers, 1u);
}

TEST(RenderGraphRealize, InitialDataBytesOutliveTheirSourceSpan) {
    // The descriptor's InitialData may point at a temporary that dies before
    // Compile; the graph copies the bytes at registration, so realize still
    // uploads the original content, never freed or reused memory.
    ResetMockCounters();
    RenderGraphBuilder Graph;
    {
        const std::vector<std::byte> Temporary(16, std::byte{0xAB});
        const auto Buffer = Graph.CreateShaderStorageBuffer(
            "TemporaryInit",
            RGShaderStorageBufferDesc{.SizeBytes   = Temporary.size(),
                                      .Usage       = RHITransientBufferUsage::ShaderRead,
                                      .InitialData = std::span<const std::byte>{Temporary}});
        Graph.AddPass<KeepReadStoragePass>({.Input = RGStorageBufferSRV{.Buffer = Buffer}});
    }
    // Best-effort reuse of the freed block so a dangling span would read
    // different bytes.
    const std::vector<std::byte> Reuse(64, std::byte{0x00});
    (void)Reuse;

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    const std::vector<std::byte> Expected(16, std::byte{0xAB});
    EXPECT_EQ(DeviceInjector::Get().LastUpload, Expected);
}

TEST(RenderGraphRealize, InitialDataSizeMismatchReturnsAnInvalidHandle) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    static constexpr std::array<std::byte, 8> Payload{};
    const auto Bad = Graph.CreateShaderStorageBuffer(
        "Bad",
        RGShaderStorageBufferDesc{.SizeBytes   = 16,
                                  .Usage       = RHITransientBufferUsage::ShaderRead,
                                  .InitialData = std::span<const std::byte>{Payload}});
    EXPECT_FALSE(Bad.IsValid());
}

// ── Parameter walk ─────────────────────────────────────────────────────────

TEST(RenderGraphParameter, PassthroughFieldsSurviveTheAggregateWalkUntouched) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto  X = MakeStorage(Graph, "X");
    const auto  Y = MakeStorage(Graph, "Y");
    Graph.AddPass<KeepFilterPass>({.Input  = RGStorageBufferSRV{.Buffer = X},
                                   .Output = RGStorageBufferUAV{.Buffer = Y}});
    Graph.AddPass<FilterPass>({.Input  = RGStorageBufferSRV{.Buffer = Y},
                               .Output = RGStorageBufferUAV{.Buffer = X},
                               .Tag    = 42,
                               .Scale  = 0.5f});
    // Anchor reads Filter's output, so the whole chain is required.
    Graph.AddPass<AnchorPass>({.Input = RGStorageBufferSRV{.Buffer = X}});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(FilterPass::LastTag, 42u);
    EXPECT_EQ(FilterPass::Constructed, 1u);
}

// ── Transfer passes ────────────────────────────────────────────────────────

TEST(RenderGraphTransfer, CopyPassBuildsWithoutAPipeline) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto  EntityId = Graph.Import(RHIRef<RHIRenderTarget>{});
    const auto  Readback = Graph.Import(RHIRef<RHIReadbackBuffer>{});
    Graph.AddPass<WriteColorPass>({.Output = RGColorRT{.Texture = EntityId}});
    Graph.AddPass<CopyPass>({.Source = RGCopySrc{.Texture = EntityId},
                             .Target = RGCopyDst{.Buffer = Readback}});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(PassNames(*List), (std::vector<StringView>{"WriteColor", "Copy"}));
    EXPECT_EQ(List->Passes.back()->GetType(), RHIPassType::Transfer);
}

// ── Compile contract ───────────────────────────────────────────────────────

TEST(RenderGraphContract, CompileIsSingleUse) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto  Imported = Graph.Import(RHIRef<RHIRenderTarget>{});
    Graph.AddPass<WriteColorPass>({.Output = RGColorRT{.Texture = Imported}});

    auto First = Graph.Compile();
    ASSERT_TRUE(First);
    auto Second = Graph.Compile();
    ASSERT_FALSE(Second);
}

TEST(RenderGraphContract, InvalidHandleIsRejectedAtCompile) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    Graph.AddPass<KeepReadStoragePass>({.Input = RGStorageBufferSRV{.Buffer = {}}});

    auto List = Graph.Compile();
    ASSERT_FALSE(List);
    EXPECT_NE(List.error().ToString().find("invalid resource handle"), String::npos);
}

TEST(RenderGraphContract, AddPassAfterCompileIsIgnored) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto  Imported = Graph.Import(RHIRef<RHIRenderTarget>{});
    Graph.AddPass<WriteColorPass>({.Output = RGColorRT{.Texture = Imported}});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(List->Passes.size(), 1u);

    // A late AddPass logs and is ignored; the compiled list is untouched.
    Graph.AddPass<WriteColorPass>({.Output = RGColorRT{.Texture = Imported}});
    EXPECT_EQ(WriteColorPass::Constructed, 1u);
}
