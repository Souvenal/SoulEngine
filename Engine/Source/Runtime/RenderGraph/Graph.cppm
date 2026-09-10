module;

export module RenderGraph:Graph;

import :Types;
import :Pass;
import :PipelineRegistry;
import RHI;
export import Core;
export import std;

export namespace SoulEngine {
} // namespace SoulEngine

namespace SoulEngine {
namespace {

// ── Aggregate walk ─────────────────────────────────────────────────────────
// Hand-rolled aggregate field enumeration (no reflection): probe the
// aggregate's brace-acceptance arity at compile time, then bind that many
// names with a structured binding. Fields without an RGViewTag (samplers,
// bindless arrays, CPU scalars) are passthrough — only views declare access.
// Cap: 16 fields per Parameter.

struct FieldProbe {
    template <typename T>
    constexpr operator T&() const noexcept;
};

template <typename T, typename... Args>
concept AggregateBraceConstructible = requires { T{std::declval<Args>()...}; };

template <typename T, std::size_t N>
consteval auto AcceptsFieldCount() -> bool {
    if constexpr (N == 0)
        return AggregateBraceConstructible<T>;
    else
        return []<std::size_t... Is>(std::index_sequence<Is...>) consteval {
            return AggregateBraceConstructible<T, decltype((void)Is, FieldProbe{})...>;
        }(std::make_index_sequence<N>{});
}

/// Highest arity the aggregate still accepts; fields all carry defaults, so
/// the maximum constructible arity is exactly the field count.
template <typename T>
consteval auto CountFields() -> std::size_t {
    return []<std::size_t... Is>(std::index_sequence<Is...>) consteval {
        std::size_t Result = 0;
        ((AcceptsFieldCount<T, Is + 1>() ? Result = Is + 1 : Result), ...);
        return Result;
    }(std::make_index_sequence<16>{});
}

template <typename FieldT, typename FnT>
auto VisitField(FieldT& Field, FnT& On) -> void {
    if constexpr (requires { typename std::remove_cvref_t<FieldT>::RGViewTag; })
        On(Field);
}

} // namespace

} // namespace SoulEngine

// Boilerplate generator for the structured-binding arities of ForEachView.
// Defined outside the export block: macros cannot be exported from a module,
// and this one must never leak to consumers.
#define RG_FOREACH_VIEW_BRANCH(ARITY, ...)                                                                  \
    if constexpr (Arity == ARITY) {                                                                         \
        auto& [__VA_ARGS__] = Value;                                                                        \
        std::apply([&](auto&... Field) { (VisitField(Field, On), ...); },                                   \
                   std::forward_as_tuple(__VA_ARGS__));                                                     \
    }

namespace SoulEngine {
namespace {

template <typename ParameterT, typename FnT>
auto ForEachView(ParameterT& Value, FnT&& On) -> void {
    constexpr std::size_t Arity = CountFields<std::remove_cvref_t<ParameterT>>();
    static_assert(Arity > 0, "RenderGraph Parameter must declare at least one field");
    if constexpr (Arity == 0) {
    } else RG_FOREACH_VIEW_BRANCH(1, M0)
    else RG_FOREACH_VIEW_BRANCH(2, M0, M1)
    else RG_FOREACH_VIEW_BRANCH(3, M0, M1, M2)
    else RG_FOREACH_VIEW_BRANCH(4, M0, M1, M2, M3)
    else RG_FOREACH_VIEW_BRANCH(5, M0, M1, M2, M3, M4)
    else RG_FOREACH_VIEW_BRANCH(6, M0, M1, M2, M3, M4, M5)
    else RG_FOREACH_VIEW_BRANCH(7, M0, M1, M2, M3, M4, M5, M6)
    else RG_FOREACH_VIEW_BRANCH(8, M0, M1, M2, M3, M4, M5, M6, M7)
    else RG_FOREACH_VIEW_BRANCH(9, M0, M1, M2, M3, M4, M5, M6, M7, M8)
    else RG_FOREACH_VIEW_BRANCH(10, M0, M1, M2, M3, M4, M5, M6, M7, M8, M9)
    else RG_FOREACH_VIEW_BRANCH(11, M0, M1, M2, M3, M4, M5, M6, M7, M8, M9, M10)
    else RG_FOREACH_VIEW_BRANCH(12, M0, M1, M2, M3, M4, M5, M6, M7, M8, M9, M10, M11)
    else RG_FOREACH_VIEW_BRANCH(13, M0, M1, M2, M3, M4, M5, M6, M7, M8, M9, M10, M11, M12)
    else RG_FOREACH_VIEW_BRANCH(14, M0, M1, M2, M3, M4, M5, M6, M7, M8, M9, M10, M11, M12, M13)
    else RG_FOREACH_VIEW_BRANCH(15, M0, M1, M2, M3, M4, M5, M6, M7, M8, M9, M10, M11, M12, M13, M14)
    else RG_FOREACH_VIEW_BRANCH(16, M0, M1, M2, M3, M4, M5, M6, M7, M8, M9, M10, M11, M12, M13, M14, M15)
    else {
        static_assert(Arity <= 16, "RenderGraph Parameter aggregate walk supports at most 16 fields");
    }
}

#undef RG_FOREACH_VIEW_BRANCH

/// The view type IS the access declaration: each view maps to exactly one
/// (usage, present) pair against its handle's resource slot.
template <typename ViewT>
[[nodiscard]] auto CollectAccess(const ViewT& View) -> RGPassNode::Access {
    if constexpr (std::same_as<ViewT, RGColorRT>)
        return {.ResourceIndex = View.Texture.Index, .Usage = RGUsage::ColorAttachmentWrite, .Present = View.Present};
    else if constexpr (std::same_as<ViewT, RGDepthRT>)
        return {.ResourceIndex = View.Texture.Index, .Usage = RGUsage::DepthAttachmentWrite};
    else if constexpr (std::same_as<ViewT, RGTextureSRV>)
        return {.ResourceIndex = View.Texture.Index, .Usage = RGUsage::SampledRead};
    else if constexpr (std::same_as<ViewT, RGStorageBufferSRV>)
        return {.ResourceIndex = View.Buffer.Index, .Usage = RGUsage::StorageBufferRead};
    else if constexpr (std::same_as<ViewT, RGStorageBufferUAV>)
        return {.ResourceIndex = View.Buffer.Index, .Usage = RGUsage::StorageBufferWrite};
    else if constexpr (std::same_as<ViewT, RGIndirectBuffer>)
        return {.ResourceIndex = View.Buffer.Index, .Usage = RGUsage::IndirectRead};
    else if constexpr (std::same_as<ViewT, RGConstantBufferSRV>)
        return {.ResourceIndex = View.Buffer.Index, .Usage = RGUsage::ConstantBufferRead};
    else if constexpr (std::same_as<ViewT, RGCopySrc>)
        return {.ResourceIndex = View.Texture.Index, .Usage = RGUsage::CopySource};
    else if constexpr (std::same_as<ViewT, RGCopyDst>)
        return {.ResourceIndex = View.Buffer.Index, .Usage = RGUsage::CopyDestination};
    else
        static_assert(sizeof(ViewT) == 0, "unsupported RenderGraph view type");
}

template <typename ViewT>
[[nodiscard]] auto ViewResourceIndex(const ViewT& View) -> Uint32 {
    if constexpr (requires { View.Texture; })
        return View.Texture.Index;
    else
        return View.Buffer.Index;
}

/// Fill one view's Ref payload from the resource entry: imported resources
/// carried their RHI object since AddPass, transients were realized at the
/// start of Compile's construction phase.
template <typename ViewT>
auto ResolveViewRef(ViewT& View, const std::vector<RGResourceEntry>& Resources) -> void {
    const auto& Entry = Resources[ViewResourceIndex(View)];
    if constexpr (std::same_as<ViewT, RGColorRT> || std::same_as<ViewT, RGDepthRT> ||
                  std::same_as<ViewT, RGTextureSRV> || std::same_as<ViewT, RGCopySrc>)
        View.Ref = Entry.Target;
    else if constexpr (std::same_as<ViewT, RGCopyDst>)
        View.Ref = Entry.Readback;
    else if constexpr (std::same_as<ViewT, RGStorageBufferSRV> || std::same_as<ViewT, RGStorageBufferUAV> ||
                       std::same_as<ViewT, RGIndirectBuffer>)
        View.Ref = Entry.StorageBuffer;
    else if constexpr (std::same_as<ViewT, RGConstantBufferSRV>)
        View.Ref = Entry.ConstantBuffer;
}

/// Called from the type-erased Build lambda at pass-construction time:
/// templates ride the module interface, so this stays visible where the
/// lambda is instantiated.
template <typename ParameterT>
auto ResolveParameterViews(ParameterT& P, const std::vector<RGResourceEntry>& Resources) -> void {
    ForEachView(P, [&](auto& Field) { ResolveViewRef(Field, Resources); });
}

} // namespace

} // namespace SoulEngine

export namespace SoulEngine {

/// Per-frame render graph. Registration (Create*/Import/AddPass) records
/// resources and passes; a single Compile() validates, orders, prunes,
/// realizes live transients, and constructs the surviving passes into a
/// RenderPassList. The graph is single-use: rebuild it every frame.
class RenderGraph {
  public:
    RenderGraph()                      = default;
    RenderGraph(const RenderGraph&)    = delete;
    auto operator=(const RenderGraph&) = delete;
    RenderGraph(RenderGraph&&)         = delete;
    auto operator=(RenderGraph&&)      = delete;

    // ── Resource registration ─────────────────────────────────────────────

    [[nodiscard]] auto CreateShaderStorageBuffer(StringView Name, const RGShaderStorageBufferDesc& Desc)
        -> RGStorageBufferHandle {
        if (m_Compiled) {
            LogError("RenderGraph: CreateShaderStorageBuffer('{}') called after Compile(); the graph is immutable "
                     "after Compile — rebuild it each frame",
                     Name);
            return {};
        }
        if (Desc.InitialData.has_value() && Desc.InitialData->size_bytes() != Desc.SizeBytes) {
            LogError("RenderGraph: storage buffer '{}': InitialData is {} bytes but SizeBytes is {}",
                     Name, Desc.InitialData->size_bytes(), Desc.SizeBytes);
            return {};
        }
        auto& Entry = m_Resources.emplace_back();
        Entry.Name                     = String(Name);
        Entry.IsTransientStorageBuffer = true;
        Entry.StorageBufferDesc        = Desc;
        if (Desc.InitialData.has_value()) {
            // The span may point at a temporary that dies before Compile;
            // realize uploads these owned bytes instead.
            Entry.OwnedInitialData.assign(Desc.InitialData->begin(), Desc.InitialData->end());
            Entry.StorageBufferDesc.InitialData = std::nullopt;
        }
        return {.Index = static_cast<Uint32>(m_Resources.size() - 1)};
    }

    [[nodiscard]] auto CreateConstantBuffer(StringView Name, const RGConstantBufferDesc& Desc)
        -> RGConstantBufferHandle {
        if (m_Compiled) {
            LogError("RenderGraph: CreateConstantBuffer('{}') called after Compile(); the graph is immutable "
                     "after Compile — rebuild it each frame",
                     Name);
            return {};
        }
        if (Desc.InitialData.has_value() && Desc.InitialData->size_bytes() != Desc.SizeBytes) {
            LogError("RenderGraph: constant buffer '{}': InitialData is {} bytes but SizeBytes is {}",
                     Name, Desc.InitialData->size_bytes(), Desc.SizeBytes);
            return {};
        }
        auto& Entry = m_Resources.emplace_back();
        Entry.Name                      = String(Name);
        Entry.IsTransientConstantBuffer = true;
        Entry.ConstantBufferDesc        = Desc;
        if (Desc.InitialData.has_value()) {
            Entry.OwnedInitialData.assign(Desc.InitialData->begin(), Desc.InitialData->end());
            Entry.ConstantBufferDesc.InitialData = std::nullopt;
        }
        return {.Index = static_cast<Uint32>(m_Resources.size() - 1)};
    }

    [[nodiscard]] auto CreateTexture(StringView Name, const RGTextureDesc& Desc) -> RGTextureHandle {
        if (m_Compiled) {
            LogError("RenderGraph: CreateTexture('{}') called after Compile(); the graph is immutable after "
                     "Compile — rebuild it each frame",
                     Name);
            return {};
        }
        m_Resources.push_back({.Name = String(Name), .IsTransientTexture = true, .TextureDesc = Desc});
        return {.Index = static_cast<Uint32>(m_Resources.size() - 1)};
    }

    /// Import an externally-owned render target. Imported resources are never
    /// realized by the graph; their Ref payload is available immediately.
    [[nodiscard]] auto Import(const RHIRef<RHIRenderTarget>& Target) -> RGTextureHandle {
        if (m_Compiled) {
            LogError("RenderGraph: Import() called after Compile(); the graph is immutable after Compile — "
                     "rebuild it each frame");
            return {};
        }
        const auto Name = Target ? String(Target->GetName()) : Format("ImportedTarget#{}", m_Resources.size());
        m_Resources.push_back({.Name = Name, .IsImported = true, .Target = Target});
        return {.Index = static_cast<Uint32>(m_Resources.size() - 1)};
    }

    /// Import an externally-owned readback buffer (GPU-to-CPU copy target).
    [[nodiscard]] auto Import(const RHIRef<RHIReadbackBuffer>& Readback) -> RGReadbackHandle {
        if (m_Compiled) {
            LogError("RenderGraph: Import() called after Compile(); the graph is immutable after Compile — "
                     "rebuild it each frame");
            return {};
        }
        const auto Name = Readback ? String(Readback->GetName()) : Format("ImportedReadback#{}", m_Resources.size());
        m_Resources.push_back({.Name = Name, .IsImported = true, .Readback = Readback});
        return {.Index = static_cast<Uint32>(m_Resources.size() - 1)};
    }

    // ── Pass registration ─────────────────────────────────────────────────

    /// Register a typed pass. The Parameter IS the resource declaration:
    /// every view-typed field contributes one access to the ordering chain;
    /// non-view fields pass through to the constructor untouched. Pass
    /// construction is deferred to Compile — registration only records.
    template <typename TPass>
    auto AddPass(typename TPass::Parameter In) -> void {
        constexpr StringView PassName = TPass::Name;
        if (m_Compiled) {
            LogError("RenderGraph: AddPass('{}') called after Compile(); the graph is immutable after Compile — "
                     "rebuild it each frame",
                     PassName);
            return;
        }

        const auto PassIndex = static_cast<Uint32>(m_Passes.size());
        auto&      Node      = m_Passes.emplace_back();
        Node.Name = String(PassName);
        if constexpr (requires { TPass::NeverPrune; })
            Node.NeverPrune = TPass::NeverPrune;
        if constexpr (requires { TPass::PresentOutput; })
            Node.PresentOutput = TPass::PresentOutput;
        if constexpr (requires { TPass::BuildPipelineRequest(); }) {
            Node.HasPipeline = true;
            Node.PipelineKey = PipelineKeyOf<TPass>();
        }

        ForEachView(In, [&](const auto& Field) {
            using FieldT = std::remove_cvref_t<decltype(Field)>;
            if constexpr (std::same_as<FieldT, RGColorRT>) {
                if (Field.Present && Field.Load) {
                    m_PendingError = Format(
                        "RenderGraph: pass '{}' declares a color attachment that is both Present and Load; the "
                        "present write is terminal, it cannot preserve prior contents",
                        PassName);
                    return;
                }
            }
            auto Access = CollectAccess(Field);
            if (Access.ResourceIndex == kRGInvalidIndex ||
                Access.ResourceIndex >= static_cast<Uint32>(m_Resources.size())) {
                m_PendingError = Format("RenderGraph: pass '{}' touches an invalid resource handle", PassName);
                return;
            }

            // Last-writer chain: a writer orders after the previous writer and
            // after every reader of the previous state; a reader orders after
            // the latest writer only. A pass never depends on itself (a view
            // pair on one resource inside one pass is that pass's own
            // sequencing concern, not the graph's).
            auto& Resource = m_Resources[Access.ResourceIndex];
            if (IsWriteUsage(Access.Usage)) {
                if (Resource.LastWriter != kRGInvalidIndex && Resource.LastWriter != PassIndex)
                    Node.Dependencies.push_back(Resource.LastWriter);
                for (const auto Reader : Resource.ReadersSinceWrite) {
                    if (Reader != PassIndex)
                        Node.Dependencies.push_back(Reader);
                }
                Resource.ReadersSinceWrite.clear();
                Resource.LastWriter = PassIndex;
            } else {
                if (Resource.LastWriter != kRGInvalidIndex && Resource.LastWriter != PassIndex)
                    Node.Dependencies.push_back(Resource.LastWriter);
                Resource.ReadersSinceWrite.push_back(PassIndex);
            }
            Node.Accesses.push_back(Access);
        });

        // The graph writes this lambda; the pass author never does. It
        // resolves the stored Parameter's view payloads (realization is
        // complete by then) and constructs the pass: pipeline passes receive
        // the registry's Ready pipeline, transfer passes the Parameter alone.
        Node.Build = [this, P = std::move(In)]() mutable -> std::expected<UPtr<IRHIPass>, ErrorMessage> {
            ResolveParameterViews(P, m_Resources);
            if constexpr (requires { TPass::BuildPipelineRequest(); }) {
                if constexpr (std::same_as<decltype(TPass::BuildPipelineRequest()), GraphicsPipelineRequest>) {
                    auto Pipeline = PipelineRegistry::Get().GetReadyGraphics(PipelineKeyOf<TPass>());
                    if (!Pipeline)
                        return std::unexpected(ErrorMessage(
                            Format("RenderGraph: pipeline for pass '{}' was Ready at prune time but is gone at "
                                   "construction",
                                   TPass::Name)));
                    return std::make_unique<TPass>(std::move(P), std::move(Pipeline));
                } else {
                    auto Pipeline = PipelineRegistry::Get().GetReadyCompute(PipelineKeyOf<TPass>());
                    if (!Pipeline)
                        return std::unexpected(ErrorMessage(
                            Format("RenderGraph: pipeline for pass '{}' was Ready at prune time but is gone at "
                                   "construction",
                                   TPass::Name)));
                    return std::make_unique<TPass>(std::move(P), std::move(Pipeline));
                }
            } else {
                return std::make_unique<TPass>(std::move(P));
            }
        };
    }

    // ── Compile ───────────────────────────────────────────────────────────

    /// Validate, order, prune, realize, and construct the frame's passes in
    /// one call. An empty RenderPassList is a valid soft-degrade result (the
    /// frame's required pass could not be built); an unexpected is a hard
    /// frame error. Single-use: a graph compiles exactly once.
    [[nodiscard]] auto Compile() -> std::expected<RenderPassList, ErrorMessage> {
        if (m_Compiled)
            return std::unexpected(
                ErrorMessage("RenderGraph: Compile() may only be called once per graph — rebuild it each frame"));
        m_Compiled = true;
        if (!m_PendingError.empty())
            return std::unexpected(ErrorMessage(std::move(m_PendingError)));

        const auto PassCount = static_cast<Uint32>(m_Passes.size());

        // Step 1: topological order over the last-writer dependencies
        // (repeated lowest-ready-index scan keeps the order deterministic:
        // independent passes run in registration order). Dependencies always
        // point at earlier passes, but the leftover check is the graph's only
        // structural validation — a leftover pass means the chain is broken.
        std::vector<Uint32> Order;
        Order.reserve(PassCount);
        {
            std::vector<bool> Done(PassCount, false);
            for (Uint32 Added = 0; Added < PassCount; ++Added) {
                bool Progress = false;
                for (Uint32 I = 0; I < PassCount; ++I) {
                    if (Done[I])
                        continue;
                    const bool Ready = std::ranges::all_of(m_Passes[I].Dependencies,
                                                           [&](Uint32 D) { return Done[D]; });
                    if (!Ready)
                        continue;
                    Done[I] = true;
                    Order.push_back(I);
                    Progress = true;
                    break;
                }
                if (!Progress) {
                    String Stuck;
                    for (Uint32 I = 0; I < PassCount; ++I) {
                        if (!Done[I])
                            Stuck += (Stuck.empty() ? "" : ", ") + m_Passes[I].Name;
                    }
                    return std::unexpected(ErrorMessage(
                        Format("RenderGraph: dependency cycle detected among passes: {}", Stuck)));
                }
            }
        }

        // Side-effect predicate, shared by Step 2 and Step 3: the pass must
        // not be pruned (NeverPrune), carries the frame Present, or writes an
        // imported resource (the write is visible outside the graph).
        const auto IsSideEffect = [&](Uint32 I) -> bool {
            const auto& Node = m_Passes[I];
            if (Node.NeverPrune || Node.PresentOutput)
                return true;
            return std::ranges::any_of(Node.Accesses, [&](const RGPassNode::Access& A) {
                return A.Present || (IsWriteUsage(A.Usage) && m_Resources[A.ResourceIndex].IsImported);
            });
        };

        // Step 2: prune. Everything a side-effect pass transitively depends
        // on is required; every other pass is dead code for this frame.
        std::vector<std::vector<Uint32>> Dependents(PassCount);
        for (Uint32 I = 0; I < PassCount; ++I)
            for (const auto D : m_Passes[I].Dependencies)
                Dependents[D].push_back(I);

        std::vector<bool>   Required(PassCount, false);
        std::vector<Uint32> Stack;
        for (Uint32 I = 0; I < PassCount; ++I) {
            if (IsSideEffect(I)) {
                Required[I] = true;
                Stack.push_back(I);
            }
        }
        while (!Stack.empty()) {
            const auto J = Stack.back();
            Stack.pop_back();
            for (const auto D : m_Passes[J].Dependencies) {
                if (!Required[D]) {
                    Required[D] = true;
                    Stack.push_back(D);
                }
            }
        }

        // Step 3: pipeline prune. A pipeline pass whose async pipeline is not
        // Ready yet is dropped, and every required pass depending on it drops
        // with it (its outputs never materialize). Dropping a side-effect
        // pass means the frame cannot produce its required output: return an
        // empty list, not an error. Failed pipelines are hard frame errors.
        std::vector<bool> Pruned(PassCount, false);
        for (Uint32 I = 0; I < PassCount; ++I) {
            if (!Required[I] || Pruned[I] || !m_Passes[I].HasPipeline)
                continue;
            auto& Registry = PipelineRegistry::Get();
            Registry.EnsureRequestStarted(m_Passes[I].PipelineKey);
            switch (Registry.GetState(m_Passes[I].PipelineKey)) {
            case RGPipelineState::Ready:
                break;
            case RGPipelineState::Failed: {
                auto Detail = Registry.GetError(m_Passes[I].PipelineKey);
                return std::unexpected(
                    (Detail ? std::move(*Detail) : ErrorMessage("pipeline compile failed"))
                        .Append(Format("RenderGraph: pipeline for pass '{}' failed to compile", m_Passes[I].Name)));
            }
            case RGPipelineState::Unknown:
                return std::unexpected(ErrorMessage(Format(
                    "RenderGraph: pipeline pass '{}' was never registered with the PipelineRegistry",
                    m_Passes[I].Name)));
            default: {  // Pending
                std::vector<Uint32> Drop{I};
                bool                DroppedSideEffect = false;
                for (std::size_t K = 0; K < Drop.size(); ++K) {
                    const auto J = Drop[K];
                    if (Pruned[J])
                        continue;
                    Pruned[J] = true;
                    if (IsSideEffect(J))
                        DroppedSideEffect = true;
                    for (const auto Dep : Dependents[J]) {
                        if (Required[Dep] && !Pruned[Dep])
                            Drop.push_back(Dep);
                    }
                }
                if (DroppedSideEffect)
                    return RenderPassList{};
                break;
            }
            }
        }

        // Step 4: realize the graph-created transients that surviving passes
        // touch. Imported resources already carry their RHI object; dead
        // transients are never allocated.
        std::vector<bool> Live(m_Resources.size(), false);
        for (Uint32 I = 0; I < PassCount; ++I) {
            if (!Required[I] || Pruned[I])
                continue;
            for (const auto& A : m_Passes[I].Accesses)
                Live[A.ResourceIndex] = true;
        }
        const bool NeedsDevice = std::ranges::any_of(std::views::iota(std::size_t{0}, m_Resources.size()),
                                                     [&](std::size_t R) {
                                                         const auto& Res = m_Resources[R];
                                                         return Live[R] && (Res.IsTransientTexture ||
                                                                            Res.IsTransientStorageBuffer ||
                                                                            Res.IsTransientConstantBuffer);
                                                     });
        if (NeedsDevice) {
            // The device is touched only when a live transient exists: a graph
            // of imported-only resources never needs one.
            auto& Device = RHIRenderDevice::Get();
            for (std::size_t R = 0; R < m_Resources.size(); ++R) {
                auto& Resource = m_Resources[R];
                if (!Live[R])
                    continue;
                if (Resource.IsTransientTexture) {
                    const auto& D = Resource.TextureDesc;
                    if (D.Width == 0 || D.Height == 0 || D.Format == RHIFormat::Unknown || D.MipLevels != 1)
                        return std::unexpected(ErrorMessage(
                            Format("RenderGraph: transient texture '{}' has an invalid descriptor", Resource.Name)));
                    auto Created = Device.CreateRenderTarget(
                        Resource.Name,
                        RHIRenderTargetDesc{.Width  = D.Width,
                                            .Height = D.Height,
                                            .Format = D.Format,
                                            .Usage  = D.AllowDepth ? RHITextureUsage::DepthStencil
                                                                   : RHITextureUsage::RenderTarget});
                    if (!Created)
                        return std::unexpected(Created.error().Append(
                            Format("RenderGraph: failed to realize transient texture '{}'", Resource.Name)));
                    Resource.Target = std::move(*Created);
                } else if (Resource.IsTransientStorageBuffer) {
                    const auto& D = Resource.StorageBufferDesc;
                    auto        Created = Device.CreateTransientShaderStorageBuffer(
                        Resource.Name,
                        RHITransientShaderStorageBufferDesc{
                            .SizeBytes   = D.SizeBytes,
                            .Usage       = D.Usage,
                            .InitialData = Resource.OwnedInitialData.empty()
                                               ? std::nullopt
                                               : std::optional<std::span<const std::byte>>{Resource.OwnedInitialData}});
                    if (!Created)
                        return std::unexpected(Created.error().Append(
                            Format("RenderGraph: failed to realize transient storage buffer '{}'", Resource.Name)));
                    Resource.StorageBuffer = std::move(*Created);
                } else if (Resource.IsTransientConstantBuffer) {
                    const auto& D = Resource.ConstantBufferDesc;
                    auto        Created = Device.CreateTransientConstantBuffer(
                        Resource.Name,
                        RHITransientConstantBufferDesc{
                            .SizeBytes   = D.SizeBytes,
                            .InitialData = Resource.OwnedInitialData.empty()
                                               ? std::nullopt
                                               : std::optional<std::span<const std::byte>>{Resource.OwnedInitialData}});
                    if (!Created)
                        return std::unexpected(Created.error().Append(
                            Format("RenderGraph: failed to realize transient constant buffer '{}'", Resource.Name)));
                    Resource.ConstantBuffer = std::move(*Created);
                }
            }
        }

        // Step 5: construct surviving passes in final order. A pass carrying
        // the Present (static PresentOutput marker or a Present access) marks
        // itself as the frame's present output.
        RenderPassList Result{};
        for (const auto I : Order) {
            if (!Required[I] || Pruned[I])
                continue;
            auto Pass = m_Passes[I].Build();
            if (!Pass)
                return std::unexpected(
                    Pass.error().Append(Format("RenderGraph: failed to construct pass '{}'", m_Passes[I].Name)));
            const bool Presents =
                m_Passes[I].PresentOutput ||
                std::ranges::any_of(m_Passes[I].Accesses, [](const RGPassNode::Access& A) { return A.Present; });
            if (Presents && (*Pass)->GetType() == RHIPassType::Graphics)
                static_cast<IRHIGraphicsPass&>(**Pass).SetPresentOutput();
            Result.Passes.push_back(std::move(*Pass));
        }
        return Result;
    }

  private:
    std::vector<RGResourceEntry> m_Resources    = {};
    std::vector<RGPassNode>      m_Passes       = {};
    bool                         m_Compiled     = false;
    String                       m_PendingError = {};
};

} // namespace SoulEngine
