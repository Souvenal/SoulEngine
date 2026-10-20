/// @file   RenderGraph.cpp
/// @brief  gtest coverage for the RenderGraph Parameter-API orchestration:
///         last-writer ordering, side-effect pruning, pipeline pruning,
///         transient realization, and the single-call Compile contract.
///
/// Mock passes are always typed: a local struct with `Name`, a nested `Parameter` whose view-typed fields ARE
/// the resource declaration, and a constructor taking the Parameter (the pipeline-side tests live in
/// PipelineRegistry.cpp). Non-pipeline mocks omit `BuildPipelineRequest`, so they never touch the
/// PipelineRegistry or the TaskGraph.

#include <entt/entt.hpp>
#include <gtest/gtest.h>
#include <magic_enum/magic_enum_flags.hpp>

import RenderGraph;
import RHI;
import WindowSystem;
import std;
import Core;

using namespace SoulEngine;

namespace {

// Test device
// Inherits the base RHIRenderDevice defaults (every other method logs "not implemented" and fails) except the
// three transient-create calls the graph's realize step makes — those succeed with an empty RHIRef, so
// Compile can walk a graph full of transients without a GPU. Each call is counted so tests can assert what
// was and was not realized.

class MockRenderDevice final : public RHIRenderDevice {
  public:
    Uint32                 CreatedTargets         = 0;
    Uint32                 CreatedStorageBuffers  = 0;
    Uint32                 CreatedConstantBuffers = 0;
    /// Usage bits of the last realized render target — lets tests assert the graph's derived usage.
    RHITextureUsage        LastTargetUsage        = RHITextureUsage::None;
    /// @brief Bytes the last transient upload carried.
    ///
    /// Lets tests assert what realize actually forwarded to the RHI.
    std::vector<std::byte> LastUpload             = {};

    [[nodiscard]] auto Initialize(IWindowSystem*) -> std::expected<void, ErrorMessage> override {
        return {};
    }
    [[nodiscard]] auto CreateTexture(StringView, const RHITextureDesc& Desc)
        -> std::expected<RHIRef<RHITexture>, ErrorMessage> override {
        ++CreatedTargets;
        LastTargetUsage = Desc.Usage;
        return RHIRef<RHITexture>{};
    }
    [[nodiscard]] auto CreateBuffer(StringView, const RHIBufferDesc& Desc)
        -> std::expected<RHIRef<RHIBuffer>, ErrorMessage> override {
        if (Desc.Usage == RHIBufferUsage::Constant) {
            ++CreatedConstantBuffers;
        } else {
            ++CreatedStorageBuffers;
        }
        return RHIRef<RHIBuffer>{};
    }
    [[nodiscard]] auto UploadBuffer(RHIRef<RHIBuffer>, std::span<const std::byte> Data)
        -> std::expected<void, ErrorMessage> override {
        LastUpload.assign(Data.begin(), Data.end());
        return {};
    }
};

/// @brief Minimal window system: satisfies the non-null check in RHIRenderDevice::Create.
///
/// The mock device never touches it.
class MockWindowSystem final : public IWindowSystem {
  public:
    [[nodiscard]] auto GetType() const -> WindowSystemType override {
        return WindowSystemType::Unknown;
    }
    [[nodiscard]] auto IsValid() const -> bool override {
        return true;
    }
    auto               Shutdown() -> void override {}
    [[nodiscard]] auto Tick() -> bool override {
        return true;
    }
    [[nodiscard]] auto GetEventDispatcher() -> entt::dispatcher& override {
        return m_Dispatcher;
    }
    [[nodiscard]] auto GetFramebufferExtent() const -> FramebufferExtent override {
        return {};
    }
    auto SetCursorMode(CursorMode) -> void override {}

  private:
    entt::dispatcher m_Dispatcher{};
};

/// @brief Registers the mock backend and creates it through the normal Create()/factory path before main()
/// runs.
///
/// Every test in this binary sees the mock device through RHIRenderDevice::Get(). The device is intentionally
/// leaked: at static-destruction time the logger and allocators may already be gone, and the OS reclaims the
/// memory either way.
struct DeviceInjector {
    DeviceInjector() {
        RHIBackendFactory::Get().Register<MockRenderDevice>("Test");
        static auto Window = MockWindowSystem{};
        const auto  Result = RHIRenderDevice::Create("Test", &Window);
        (void)Result;
    }
    [[nodiscard]] static auto Get() -> MockRenderDevice& {
        return static_cast<MockRenderDevice&>(RHIRenderDevice::Get());
    }
};
const auto G_InjectDevice = DeviceInjector{};

// Mock passes
// Each mock counts its constructions: passes are born at AddPass, so every registered pass counts exactly
// once, pruned or not. Tests assert pruning through the compiled list (via GetName) and realization through
// the device counters.

struct WriteColorPass final : IRHIGraphicsPass {
    static constexpr StringView Name        = "WriteColor";
    static inline Uint32        Constructed = 0;
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    struct Parameter {
        RGTextureView<RHITextureUsage::RenderTarget, RHIAccess::Write> Output;
    };

    WriteColorPass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {
        ++Constructed;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

struct ReadColorPass final : IRHIGraphicsPass {
    static constexpr StringView Name        = "ReadColor";
    static inline Uint32        Constructed = 0;
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    struct Parameter {
        RGTextureView<RHITextureUsage::Sampled, RHIAccess::Read> Input;
    };

    ReadColorPass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {
        ++Constructed;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

struct WriteStoragePass final : IRHITransferPass {
    static constexpr StringView Name        = "WriteStorage";
    static inline Uint32        Constructed = 0;
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    struct Parameter {
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Write> Output;
    };

    WriteStoragePass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {
        ++Constructed;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

struct ReadStoragePass final : IRHITransferPass {
    static constexpr StringView Name        = "ReadStorage";
    static inline Uint32        Constructed = 0;
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    struct Parameter {
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read> Input;
    };

    ReadStoragePass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {
        ++Constructed;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

/// NeverPrune variant: pruning must keep this pass even with no downstream consumers.
struct KeepReadStoragePass final : IRHITransferPass {
    static constexpr StringView Name        = "KeepReadStorage";
    static constexpr bool       NeverPrune  = true;
    static inline Uint32        Constructed = 0;
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    struct Parameter {
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read> Input;
    };

    KeepReadStoragePass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {
        ++Constructed;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

/// @brief Compute pass mixing views and passthrough CPU scalars.
///
/// The aggregate walk must declare access for the views only and leave the scalars untouched.
struct FilterPass final : IRHIComputePass {
    static constexpr StringView Name        = "Filter";
    static inline Uint32        Constructed = 0;
    static inline Uint32        LastTag     = 0;
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    struct Parameter {
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>  Input;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Write> Output;
        Uint32                                                  Tag   = 0;
        Float32                                                 Scale = 1.0f;
    };

    FilterPass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {
        ++Constructed;
        LastTag = m_Parameter->Tag;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

struct KeepFilterPass final : IRHIComputePass {
    static constexpr StringView Name        = "KeepFilter";
    static constexpr bool       NeverPrune  = true;
    static inline Uint32        Constructed = 0;
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    struct Parameter {
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>  Input;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Write> Output;
    };

    KeepFilterPass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {
        ++Constructed;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

struct ReadConstantPass final : IRHIComputePass {
    static constexpr StringView Name        = "ReadConstant";
    static constexpr bool       NeverPrune  = true;
    static inline Uint32        Constructed = 0;
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    struct Parameter {
        RGBufferView<RHIBufferUsage::Constant, RHIAccess::Read> Input;
    };

    ReadConstantPass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {
        ++Constructed;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

struct CopyPass final : IRHITransferPass {
    static constexpr StringView Name        = "Copy";
    static inline Uint32        Constructed = 0;
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    struct Parameter {
        RGTextureView<RHITextureUsage::TransferSrc, RHIAccess::Read> Source;
        RGBufferView<RHIBufferUsage::TransferDst, RHIAccess::Write>  Target;
    };

    CopyPass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {
        ++Constructed;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

/// @brief Graphics pass mixing a storage read with an imported color write.
///
/// The imported write is the side effect that anchors its producer chain.
struct BridgePass final : IRHIGraphicsPass {
    static constexpr StringView Name = "Bridge";
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    struct Parameter {
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>         Input;
        RGTextureView<RHITextureUsage::RenderTarget, RHIAccess::Write> Output;
    };

    BridgePass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {}
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

/// Depth-attachment writer mock: contributes DepthStencil to the texture's derived usage.
struct WriteDepthPass final : IRHIGraphicsPass {
    static constexpr StringView Name        = "WriteDepth";
    static inline Uint32        Constructed = 0;
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    struct Parameter {
        RGTextureView<RHITextureUsage::DepthStencil, RHIAccess::Write> Depth;
    };

    WriteDepthPass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {
        ++Constructed;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

/// Storage-image writer mock: contributes Storage to the texture's derived usage while it survives pruning.
struct WriteStorageTexturePass final : IRHIComputePass {
    static constexpr StringView Name        = "WriteStorageTexture";
    static inline Uint32        Constructed = 0;
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    struct Parameter {
        RGTextureView<RHITextureUsage::Storage, RHIAccess::Write> Output;
    };

    WriteStorageTexturePass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {
        ++Constructed;
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

/// NeverPrune storage reader used as the frame anchor in ordering tests.
struct AnchorPass final : IRHITransferPass {
    static constexpr StringView Name       = "Anchor";
    static constexpr bool       NeverPrune = true;
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    struct Parameter {
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read> Input;
    };

    AnchorPass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {}
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

auto ResetMockCounters() -> void {
    WriteColorPass::Constructed                  = 0;
    WriteDepthPass::Constructed                  = 0;
    WriteStorageTexturePass::Constructed         = 0;
    ReadColorPass::Constructed                   = 0;
    WriteStoragePass::Constructed                = 0;
    ReadStoragePass::Constructed                 = 0;
    KeepReadStoragePass::Constructed             = 0;
    FilterPass::Constructed                      = 0;
    KeepFilterPass::Constructed                  = 0;
    ReadConstantPass::Constructed                = 0;
    CopyPass::Constructed                        = 0;
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
    Names.reserve(List.Entries.size());
    for (const auto& Entry : List.Entries)
        Names.push_back(Entry.Pass->GetName());
    return Names;
}

[[nodiscard]] auto MakeStorage(RenderGraphBuilder& Graph, StringView Name, Uint64 SizeBytes = 16) -> RGBuffer {
    return Graph.CreateBuffer(
        Name,
        RHIBufferDesc{.SizeBytes = SizeBytes, .Usage = RHIBufferUsage::Storage, .Lifetime = RHILifetime::Transient});
}

} // namespace
// Ordering

TEST(RenderGraphOrder, IndependentPassesKeepRegistrationOrder) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto         X = MakeStorage(Graph, "X");
    const auto         Y = MakeStorage(Graph, "Y");
    Graph.AddPass<WriteStoragePass>({.Output = X});
    Graph.AddPass<WriteStoragePass>({.Output = Y});
    // A NeverPrune reader anchors both producers in the frame.
    Graph.AddPass<KeepFilterPass>({.Input = X, .Output = Y});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(PassNames(*List), (std::vector<StringView>{"WriteStorage", "WriteStorage", "KeepFilter"}));
}

TEST(RenderGraphOrder, ReaderOrdersAfterLastWriterOnly) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto         X = MakeStorage(Graph, "X");
    const auto         Y = MakeStorage(Graph, "Y");
    Graph.AddPass<WriteStoragePass>({.Output = X}); // X writer
    Graph.AddPass<FilterPass>({.Input  = X,         // reads X, writes Y
                               .Output = Y});
    Graph.AddPass<KeepFilterPass>({.Input  = Y, // depends on Filter only
                                   .Output = X});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(PassNames(*List), (std::vector<StringView>{"WriteStorage", "Filter", "KeepFilter"}));
}

TEST(RenderGraphOrder, MultipleWritersChainInRegistrationOrder) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto         X = MakeStorage(Graph, "X");
    Graph.AddPass<WriteStoragePass>({.Output = X});
    Graph.AddPass<FilterPass>({.Input = X, .Output = X});
    Graph.AddPass<KeepReadStoragePass>({.Input = X});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(PassNames(*List), (std::vector<StringView>{"WriteStorage", "Filter", "KeepReadStorage"}));
}
// Pruning

TEST(RenderGraphPrune, DeadProducerIsNeverRealized) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto         Dead = MakeStorage(Graph, "Dead");
    const auto         Live = MakeStorage(Graph, "Live");
    Graph.AddPass<WriteStoragePass>({.Output = Dead}); // nobody reads Dead
    Graph.AddPass<KeepReadStoragePass>({.Input = Live});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(PassNames(*List), (std::vector<StringView>{"KeepReadStorage"}));
    // Dead's producer is pruned: only Live is realized.
    EXPECT_EQ(DeviceInjector::Get().CreatedStorageBuffers, 1u);
}

TEST(RenderGraphPrune, PruneIsTransitiveOverTheDependencyChain) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto         X = MakeStorage(Graph, "X");
    const auto         Y = MakeStorage(Graph, "Y");
    Graph.AddPass<WriteStoragePass>({.Output = X});
    Graph.AddPass<FilterPass>({.Input = X, .Output = Y});
    Graph.AddPass<ReadStoragePass>({.Input = Y}); // no side effect
    // Independent side-effect pass keeps the frame non-empty.
    const auto Imported = Graph.Import(RHIRef<RHITexture>{});
    Graph.AddPass<WriteColorPass>({.Output = Imported});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(PassNames(*List), (std::vector<StringView>{"WriteColor"}));
    // The whole pruned chain realizes nothing.
    EXPECT_EQ(DeviceInjector::Get().CreatedStorageBuffers, 0u);
}

TEST(RenderGraphPrune, ImportedWriteKeepsItsProducerChainAlive) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto         X        = MakeStorage(Graph, "X");
    const auto         Imported = Graph.Import(RHIRef<RHITexture>{});
    Graph.AddPass<WriteStoragePass>({.Output = X});
    Graph.AddPass<FilterPass>({.Input = X, .Output = X});
    // Reads X and writes an imported target: the write is visible outside the graph, so the whole chain
    // survives.
    Graph.AddPass<BridgePass>({.Input = X, .Output = Imported});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(PassNames(*List), (std::vector<StringView>{"WriteStorage", "Filter", "Bridge"}));
    EXPECT_EQ(DeviceInjector::Get().CreatedStorageBuffers, 1u);
}
/// @brief Generic non-prunable transfer pass used to anchor graph behavior tests.
struct NeverPruneTransferPass final : IRHITransferPass {
    static constexpr StringView Name       = "NeverPruneTransfer";
    static constexpr bool       NeverPrune = true;

    [[nodiscard]] auto GetName() const noexcept -> StringView override {
        return Name;
    }

    struct Parameter {
        RGTextureView<RHITextureUsage::TransferSrc, RHIAccess::Read> Source;
    };

    explicit NeverPruneTransferPass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {}

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

// Generic transfer-pass anchoring

TEST(RenderGraphTransfer, NeverPruneSurvivesWithoutConsumers) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto         Target = Graph.Import(RHIRef<RHITexture>{});
    Graph.AddPass<NeverPruneTransferPass>({.Source = Target});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    ASSERT_EQ(List->Entries.size(), 1u);
    EXPECT_EQ(List->Entries.front().Pass->GetName(), "NeverPruneTransfer");
}

TEST(RenderGraphTransfer, OrdersAfterTheSourceWriter) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto Target = Graph.CreateTexture("SceneColor",
                                            {.Width  = 8,
                                             .Height = 8,
                                             .Format = RHIFormat::B8G8R8A8_UNORM,
                                             .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::TransferSrc});
    Graph.AddPass<WriteColorPass>({.Output = Target});
    Graph.AddPass<NeverPruneTransferPass>({.Source = Target});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(PassNames(*List), (std::vector<StringView>{"WriteColor", "NeverPruneTransfer"}));
}

// Creation usage
// Usage is declared at creation and flows to the device verbatim; every surviving view's role is checked
// against it at Compile.

TEST(RenderGraphCreationUsage, CreationUsageFlowsThroughVerbatim) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto Target = Graph.CreateTexture("SceneColor",
                                            {.Width  = 8,
                                             .Height = 8,
                                             .Format = RHIFormat::B8G8R8A8_UNORM,
                                             .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::TransferSrc});
    Graph.AddPass<WriteColorPass>({.Output = Target});
    Graph.AddPass<NeverPruneTransferPass>({.Source = Target});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    ASSERT_EQ(DeviceInjector::Get().CreatedTargets, 1u);
    const auto Usage = DeviceInjector::Get().LastTargetUsage;
    EXPECT_TRUE(magic_enum::enum_flags_test(Usage, RHITextureUsage::RenderTarget));
    EXPECT_TRUE(magic_enum::enum_flags_test(Usage, RHITextureUsage::TransferSrc));
    EXPECT_FALSE(magic_enum::enum_flags_test(Usage, RHITextureUsage::Storage));
}

TEST(RenderGraphCreationUsage, SurvivingViewOutsideCreationUsageIsAFrameError) {
    // The transfer pass is NeverPrune, so its TransferSrc read always survives — and the texture was created
    // RenderTarget-only. Compile must say why.
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto         Target = Graph.CreateTexture(
        "SceneColor",
        {.Width = 8, .Height = 8, .Format = RHIFormat::B8G8R8A8_UNORM, .Usage = RHITextureUsage::RenderTarget});
    Graph.AddPass<WriteColorPass>({.Output = Target});
    Graph.AddPass<NeverPruneTransferPass>({.Source = Target});

    auto List = Graph.Compile();
    ASSERT_FALSE(List);
    EXPECT_NE(List.error().ToString().find("creation usage"), String::npos);
}

TEST(RenderGraphCreationUsage, PrunedPassEscapesTheUsageCheck) {
    // A dead pass may declare a role the creation usage does not cover: it never runs, so its declaration is
    // not checked.
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto Target = Graph.CreateTexture("SceneColor",
                                            {.Width  = 8,
                                             .Height = 8,
                                             .Format = RHIFormat::B8G8R8A8_UNORM,
                                             .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::TransferSrc});
    Graph.AddPass<WriteColorPass>({.Output = Target});
    // No side effect and nothing required depends on it: this sampled read is dead code; Sampled is outside
    // the creation usage but unchecked.
    Graph.AddPass<ReadColorPass>({.Input = Target});
    Graph.AddPass<NeverPruneTransferPass>({.Source = Target});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    ASSERT_EQ(DeviceInjector::Get().CreatedTargets, 1u);
    const auto Usage = DeviceInjector::Get().LastTargetUsage;
    EXPECT_FALSE(magic_enum::enum_flags_test(Usage, RHITextureUsage::Sampled));
}
// Render-target pool

TEST(RenderGraphPool, ReusesAcrossBuilderLifetimes) {
    ResetMockCounters();
    const auto MakeFrame = [] {
        RenderGraphBuilder Graph;
        const auto         Target =
            Graph.CreateTexture("SceneColor",
                                {.Width  = 8,
                                 .Height = 8,
                                 .Format = RHIFormat::B8G8R8A8_UNORM,
                                 .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::TransferSrc});
        Graph.AddPass<WriteColorPass>({.Output = Target});
        Graph.AddPass<NeverPruneTransferPass>({.Source = Target});
        auto List = Graph.Compile();
        EXPECT_TRUE(List);
        // Builder dies here: pooled refs return immediately (no GPU gate).
    };
    MakeFrame();
    MakeFrame();
    // Second frame's identical desc must hit the pool: no new creation.
    EXPECT_EQ(DeviceInjector::Get().CreatedTargets, 1u);
}

TEST(RenderGraphPool, KeyDistinguishesCreationUsage) {
    ResetMockCounters();
    const auto MakeFrame = [](bool Storage) {
        RenderGraphBuilder Graph;
        const auto         Target =
            Graph.CreateTexture("Shared",
                                {.Width  = 8,
                                 .Height = 8,
                                 .Format = RHIFormat::B8G8R8A8_UNORM,
                                 .Usage  = (Storage ? RHITextureUsage::Storage : RHITextureUsage::RenderTarget) |
                                           RHITextureUsage::TransferSrc});
        if (Storage)
            Graph.AddPass<WriteStorageTexturePass>({.Output = Target});
        else
            Graph.AddPass<WriteColorPass>({.Output = Target});
        Graph.AddPass<NeverPruneTransferPass>({.Source = Target});
        auto List = Graph.Compile();
        EXPECT_TRUE(List);
    };
    MakeFrame(false); // RenderTarget | TransferSrc
    MakeFrame(true);  // Storage | TransferSrc — different bucket
    EXPECT_EQ(DeviceInjector::Get().CreatedTargets, 2u);
}

TEST(RenderGraphPool, TickEvictsIdleEntries) {
    ResetMockCounters();
    {
        RenderGraphBuilder Graph;
        const auto         Target =
            Graph.CreateTexture("SceneColor",
                                {.Width  = 8,
                                 .Height = 8,
                                 .Format = RHIFormat::B8G8R8A8_UNORM,
                                 .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::TransferSrc});
        Graph.AddPass<WriteColorPass>({.Output = Target});
        Graph.AddPass<NeverPruneTransferPass>({.Source = Target});
        auto List = Graph.Compile();
        ASSERT_TRUE(List);
    }
    ASSERT_EQ(RenderGraph::Get().GetPool().GetFreeEntryCount(), 1u);
    // kRGEvictIdleFrames is module-internal (RenderGraph:TexturePool is not re-exported); this mirror must
    // match RGTexturePool.cppm.
    constexpr Uint32 EvictIdleFrames = 10;
    for (Uint32 I = 0; I < EvictIdleFrames; ++I)
        RenderGraph::Get().Tick();
    EXPECT_EQ(RenderGraph::Get().GetPool().GetFreeEntryCount(), 0u);
    // After eviction the same desc is a miss again.
    {
        RenderGraphBuilder Graph;
        const auto         Target =
            Graph.CreateTexture("SceneColor",
                                {.Width  = 8,
                                 .Height = 8,
                                 .Format = RHIFormat::B8G8R8A8_UNORM,
                                 .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::TransferSrc});
        Graph.AddPass<WriteColorPass>({.Output = Target});
        Graph.AddPass<NeverPruneTransferPass>({.Source = Target});
        auto List = Graph.Compile();
        ASSERT_TRUE(List);
    }
    EXPECT_EQ(DeviceInjector::Get().CreatedTargets, 2u);
}

TEST(RenderGraphCreationUsage, DepthAttachmentCreationUsageFlowsThrough) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto Target = Graph.CreateTexture("Depth",
                                            {.Width  = 8,
                                             .Height = 8,
                                             .Format = RHIFormat::D32_SFLOAT,
                                             .Usage  = RHITextureUsage::DepthStencil | RHITextureUsage::TransferSrc});
    Graph.AddPass<WriteDepthPass>({.Depth = Target});
    // A depth write alone is dead code; the NeverPrune reader anchors it.
    Graph.AddPass<NeverPruneTransferPass>({.Source = Target});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    ASSERT_EQ(DeviceInjector::Get().CreatedTargets, 1u);
    const auto Usage = DeviceInjector::Get().LastTargetUsage;
    EXPECT_TRUE(magic_enum::enum_flags_test(Usage, RHITextureUsage::DepthStencil));
    EXPECT_FALSE(magic_enum::enum_flags_test(Usage, RHITextureUsage::RenderTarget));
}

TEST(RenderGraphCreationUsage, StorageImageCreationUsageFlowsThrough) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto         Target = Graph.CreateTexture("RT/Output",
                                                    {.Width  = 8,
                                                     .Height = 8,
                                                     .Format = RHIFormat::B8G8R8A8_UNORM,
                                                     .Usage = RHITextureUsage::Storage | RHITextureUsage::TransferSrc});
    Graph.AddPass<WriteStorageTexturePass>({.Output = Target});
    Graph.AddPass<NeverPruneTransferPass>({.Source = Target});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    ASSERT_EQ(DeviceInjector::Get().CreatedTargets, 1u);
    const auto Usage = DeviceInjector::Get().LastTargetUsage;
    EXPECT_TRUE(magic_enum::enum_flags_test(Usage, RHITextureUsage::Storage));
    EXPECT_TRUE(magic_enum::enum_flags_test(Usage, RHITextureUsage::TransferSrc));
    EXPECT_FALSE(magic_enum::enum_flags_test(Usage, RHITextureUsage::RenderTarget));
}
// Realization

TEST(RenderGraphRealize, DeadTransientsAreNeverRealized) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto         Dead = MakeStorage(Graph, "Dead");
    const auto         Live = MakeStorage(Graph, "Live");
    Graph.AddPass<WriteStoragePass>({.Output = Dead});
    Graph.AddPass<KeepReadStoragePass>({.Input = Live});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(DeviceInjector::Get().CreatedStorageBuffers, 1u);
}

TEST(RenderGraphRealize, TransientWithInitialDataIsReadableWithoutAWriter) {
    ResetMockCounters();
    RenderGraphBuilder                         Graph;
    static constexpr std::array<std::byte, 16> Payload{};
    const auto Buffer = Graph.CreateBuffer("UploadTable",
                                           RHIBufferDesc{.SizeBytes = Payload.size(),
                                                         .Usage     = RHIBufferUsage::Storage,
                                                         .Lifetime  = RHILifetime::Transient},
                                           std::span<const std::byte>{Payload});
    Graph.AddPass<KeepReadStoragePass>({.Input = Buffer});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(PassNames(*List), (std::vector<StringView>{"KeepReadStorage"}));
    EXPECT_EQ(DeviceInjector::Get().CreatedStorageBuffers, 1u);
}

TEST(RenderGraphRealize, TransientWithoutInitialDataIsAlsoReadableWithoutAWriter) {
    // No InitialData means undefined scratch contents; the graph does not police that — the reader simply
    // reads whatever the GPU left there.
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto         Scratch = MakeStorage(Graph, "Scratch");
    Graph.AddPass<KeepReadStoragePass>({.Input = Scratch});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(DeviceInjector::Get().CreatedStorageBuffers, 1u);
}

TEST(RenderGraphRealize, ConstantBufferWithInitialDataRealizesOnce) {
    ResetMockCounters();
    RenderGraphBuilder                         Graph;
    static constexpr std::array<std::byte, 32> Payload{};
    const auto CB = Graph.CreateBuffer("FrameConstants",
                                       RHIBufferDesc{.SizeBytes = Payload.size(),
                                                     .Usage     = RHIBufferUsage::Constant,
                                                     .Lifetime  = RHILifetime::Transient},
                                       std::span<const std::byte>{Payload});
    Graph.AddPass<ReadConstantPass>({.Input = CB});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(DeviceInjector::Get().CreatedConstantBuffers, 1u);
}

TEST(RenderGraphRealize, InitialDataBytesOutliveTheirSourceSpan) {
    // The descriptor's InitialData may point at a temporary that dies before Compile; the graph copies the
    // bytes at registration, so realize still uploads the original content, never freed or reused memory.
    ResetMockCounters();
    RenderGraphBuilder Graph;
    {
        const std::vector<std::byte> Temporary(16, std::byte{0xAB});
        const auto                   Buffer = Graph.CreateBuffer("TemporaryInit",
                                                                 RHIBufferDesc{.SizeBytes = Temporary.size(),
                                                                               .Usage     = RHIBufferUsage::Storage,
                                                                               .Lifetime  = RHILifetime::Transient},
                                                                 std::span<const std::byte>{Temporary});
        Graph.AddPass<KeepReadStoragePass>({.Input = Buffer});
    }
    // Best-effort reuse of the freed block so a dangling span would read different bytes.
    const std::vector<std::byte> Reuse(64, std::byte{0x00});
    (void)Reuse;

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    const std::vector<std::byte> Expected(16, std::byte{0xAB});
    EXPECT_EQ(DeviceInjector::Get().LastUpload, Expected);
}

TEST(RenderGraphRealize, InitialDataSizeMismatchReturnsAnInvalidHandle) {
    ResetMockCounters();
    RenderGraphBuilder                        Graph;
    static constexpr std::array<std::byte, 8> Payload{};
    const auto                                Bad = Graph.CreateBuffer(
        "Bad",
        RHIBufferDesc{.SizeBytes = 16, .Usage = RHIBufferUsage::Storage, .Lifetime = RHILifetime::Transient},
        std::span<const std::byte>{Payload});
    EXPECT_FALSE(Bad.IsValid());
}
// Parameter walk

TEST(RenderGraphParameter, PassthroughFieldsSurviveTheAggregateWalkUntouched) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto         X = MakeStorage(Graph, "X");
    const auto         Y = MakeStorage(Graph, "Y");
    Graph.AddPass<KeepFilterPass>({.Input = X, .Output = Y});
    Graph.AddPass<FilterPass>({.Input = Y, .Output = X, .Tag = 42, .Scale = 0.5f});
    // Anchor reads Filter's output, so the whole chain is required.
    Graph.AddPass<AnchorPass>({.Input = X});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(FilterPass::LastTag, 42u);
    EXPECT_EQ(FilterPass::Constructed, 1u);
}
// Transfer passes

TEST(RenderGraphTransfer, CopyPassBuildsWithoutAPipeline) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto         EntityId = Graph.Import(RHIRef<RHITexture>{});
    const auto         Readback = Graph.Import(RHIRef<RHIBuffer>{});
    Graph.AddPass<WriteColorPass>({.Output = EntityId});
    Graph.AddPass<CopyPass>({.Source = EntityId, .Target = Readback});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(PassNames(*List), (std::vector<StringView>{"WriteColor", "Copy"}));
    EXPECT_EQ(List->Entries.back().Pass->GetType(), RHIPassType::Transfer);
}
// Compile contract

TEST(RenderGraphContract, CompileIsSingleUse) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto         Imported = Graph.Import(RHIRef<RHITexture>{});
    Graph.AddPass<WriteColorPass>({.Output = Imported});

    auto First = Graph.Compile();
    ASSERT_TRUE(First);
    auto Second = Graph.Compile();
    ASSERT_FALSE(Second);
}

TEST(RenderGraphContract, InvalidHandleIsRejectedAtCompile) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    Graph.AddPass<KeepReadStoragePass>({.Input = {}});

    auto List = Graph.Compile();
    ASSERT_FALSE(List);
    EXPECT_NE(List.error().ToString().find("invalid resource handle"), String::npos);
}

TEST(RenderGraphContract, AddPassAfterCompileIsIgnored) {
    ResetMockCounters();
    RenderGraphBuilder Graph;
    const auto         Imported = Graph.Import(RHIRef<RHITexture>{});
    Graph.AddPass<WriteColorPass>({.Output = Imported});

    auto List = Graph.Compile();
    ASSERT_TRUE(List);
    EXPECT_EQ(List->Entries.size(), 1u);

    // A late AddPass logs and is ignored; the compiled list is untouched.
    Graph.AddPass<WriteColorPass>({.Output = Imported});
    EXPECT_EQ(WriteColorPass::Constructed, 1u);
}
