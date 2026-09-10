/// @file   RenderGraphGraph.cpp
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
    /// Bytes the last transient upload carried — lets tests assert what
    /// realize actually forwarded to the RHI.
    std::vector<std::byte> LastUpload = {};

    [[nodiscard]] auto Initialize(IWindowSystem*) -> std::expected<void, ErrorMessage> override {
        return {};
    }
    [[nodiscard]] auto CreateRenderTarget(StringView, const RHIRenderTargetDesc&)
        -> std::expected<RHIRef<RHIRenderTarget>, ErrorMessage> override {
        ++CreatedTargets;
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

/// Static PresentOutput marker: the pass carries the frame Present
/// regardless of view bits (its SceneColor is a Load attachment).
struct OutlinePass final : IRHIGraphicsPass {
    static constexpr StringView Name          = "Outline";
    static constexpr bool       PresentOutput = true;
    static inline Uint32        Constructed   = 0;

    struct Parameter {
        RGColorRT SceneColor = {.Load = true};
    };

    OutlinePass(Parameter In)
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
    OutlinePass::Constructed         = 0;
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
}

/// Pass names in compiled order — the observable result of ordering + pruning.
[[nodiscard]] auto PassNames(const RenderPassList& List) -> std::vector<StringView> {
    std::vector<StringView> Names;
    Names.reserve(List.Passes.size());
    for (const auto& Pass : List.Passes)
        Names.push_back(Pass->GetName());
    return Names;
}

[[nodiscard]] auto MakeStorage(RenderGraph& Graph, StringView Name, Uint64 SizeBytes = 16) -> RGStorageBufferHandle {
    return Graph.CreateShaderStorageBuffer(
        Name, RGShaderStorageBufferDesc{.SizeBytes = SizeBytes, .Usage = RHITransientBufferUsage::ShaderRead});
}

} // namespace

// ── Ordering ───────────────────────────────────────────────────────────────

TEST(RenderGraphOrder, IndependentPassesKeepRegistrationOrder) {
    ResetMockCounters();
    RenderGraph Graph;
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
    RenderGraph Graph;
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
    RenderGraph Graph;
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
    RenderGraph Graph;
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
    RenderGraph Graph;
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
    RenderGraph Graph;
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

// ── Present ────────────────────────────────────────────────────────────────

TEST(RenderGraphPresent, PresentAccessMarksTheGraphicsPassAsPresentOutput) {
    ResetMockCounters();
    RenderGraph Graph;
    const auto  Target = Graph.Import(RHIRef<RHIRenderTarget>{});
    Graph.AddPass<WriteColorPass>({.Output = RGColorRT{.Texture = Target, .Present = true}});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    ASSERT_EQ(List->Passes.size(), 1u);
    EXPECT_TRUE(static_cast<IRHIGraphicsPass*>(List->Passes.front().get())->HasPresentOutput());
}

TEST(RenderGraphPresent, StaticPresentOutputMarkerMarksThePass) {
    ResetMockCounters();
    RenderGraph Graph;
    const auto  Target = Graph.Import(RHIRef<RHIRenderTarget>{});
    Graph.AddPass<OutlinePass>({.SceneColor = RGColorRT{.Texture = Target, .Load = true}});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    ASSERT_EQ(List->Passes.size(), 1u);
    EXPECT_TRUE(static_cast<IRHIGraphicsPass*>(List->Passes.front().get())->HasPresentOutput());
}

TEST(RenderGraphPresent, PlainImportedWriteIsNotMarkedAsPresentOutput) {
    ResetMockCounters();
    RenderGraph Graph;
    const auto  Target = Graph.Import(RHIRef<RHIRenderTarget>{});
    Graph.AddPass<WriteColorPass>({.Output = RGColorRT{.Texture = Target}});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    ASSERT_EQ(List->Passes.size(), 1u);
    EXPECT_FALSE(static_cast<IRHIGraphicsPass*>(List->Passes.front().get())->HasPresentOutput());
}

TEST(RenderGraphPresent, PresentAndLoadOnOneAttachmentIsRejected) {
    ResetMockCounters();
    RenderGraph Graph;
    const auto  Target = Graph.Import(RHIRef<RHIRenderTarget>{});
    Graph.AddPass<WriteColorPass>({.Output = RGColorRT{.Texture = Target, .Present = true, .Load = true}});

    auto List = Graph.Compile();
    ASSERT_FALSE(List);
    EXPECT_NE(List.error().ToString().find("WriteColor"), String::npos);
}

// ── Realization ────────────────────────────────────────────────────────────

TEST(RenderGraphRealize, DeadTransientsAreNeverRealized) {
    ResetMockCounters();
    RenderGraph Graph;
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
    RenderGraph Graph;
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
    RenderGraph Graph;
    const auto  Scratch = MakeStorage(Graph, "Scratch");
    Graph.AddPass<KeepReadStoragePass>({.Input = RGStorageBufferSRV{.Buffer = Scratch}});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(DeviceInjector::Get().CreatedStorageBuffers, 1u);
}

TEST(RenderGraphRealize, ConstantBufferWithInitialDataRealizesOnce) {
    ResetMockCounters();
    RenderGraph Graph;
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
    RenderGraph Graph;
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
    RenderGraph Graph;
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
    RenderGraph Graph;
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
    RenderGraph Graph;
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
    RenderGraph Graph;
    const auto  Imported = Graph.Import(RHIRef<RHIRenderTarget>{});
    Graph.AddPass<WriteColorPass>({.Output = RGColorRT{.Texture = Imported}});

    auto First = Graph.Compile();
    ASSERT_TRUE(First);
    auto Second = Graph.Compile();
    ASSERT_FALSE(Second);
}

TEST(RenderGraphContract, InvalidHandleIsRejectedAtCompile) {
    ResetMockCounters();
    RenderGraph Graph;
    Graph.AddPass<KeepReadStoragePass>({.Input = RGStorageBufferSRV{.Buffer = {}}});

    auto List = Graph.Compile();
    ASSERT_FALSE(List);
    EXPECT_NE(List.error().ToString().find("invalid resource handle"), String::npos);
}

TEST(RenderGraphContract, AddPassAfterCompileIsIgnored) {
    ResetMockCounters();
    RenderGraph Graph;
    const auto  Imported = Graph.Import(RHIRef<RHIRenderTarget>{});
    Graph.AddPass<WriteColorPass>({.Output = RGColorRT{.Texture = Imported}});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(List->Passes.size(), 1u);

    // A late AddPass logs and is ignored; the compiled list is untouched.
    Graph.AddPass<WriteColorPass>({.Output = RGColorRT{.Texture = Imported}});
    EXPECT_EQ(WriteColorPass::Constructed, 1u);
}
