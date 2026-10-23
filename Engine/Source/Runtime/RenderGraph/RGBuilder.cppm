module;

// Clang 22 named-modules bug: concepts are shadowed through the std re-export chain (import Core/RHI);
// textual include restores them.
#include <concepts>

// magic_enum::enum_flags_test is not exported by the v0.9.8 module.
#include <magic_enum/magic_enum_flags.hpp>

export module RenderGraph.Builder;

import RenderGraph.Types;
import RenderGraph.PipelineRegistry;
import RenderGraph.Runtime;
import RenderGraph.SyncPlan;
import RHI;
import Core;
import std;

namespace SoulEngine {

// The templates below are exported on purpose: the exported AddPass template is instantiated inside renderer
// module units, and at that instantiation point only exported declarations of this partition are visible.
// Anonymous namespaces here would make every cross-module AddPass call fail to compile.

// Aggregate walk

// Hand-rolled aggregate field enumeration (no reflection): probe the aggregate's brace-acceptance arity at
// compile time, then bind that many names with a structured binding. Fields not deriving from one of the two
// view records (samplers, bindless arrays, CPU scalars) are passthrough — only views declare access. Cap: 16
// fields per Parameter.

export struct FieldProbe {
    template <typename T>
    constexpr operator T&() const noexcept;
};

export template <typename T, typename... Args>
concept AggregateBraceConstructible = requires { T{std::declval<Args>()...}; };

export template <typename T, std::size_t N>
consteval auto AcceptsFieldCount() -> bool {
    if constexpr (N == 0)
        return AggregateBraceConstructible<T>;
    else
        return []<std::size_t... Is>(std::index_sequence<Is...>) consteval {
            return AggregateBraceConstructible<T, decltype((void)Is, FieldProbe{})...>;
        }(std::make_index_sequence<N>{});
}

/// @brief Highest arity the aggregate still accepts.
///
/// Fields all carry defaults, so the maximum constructible arity is exactly the field count.
export template <typename T>
consteval auto CountFields() -> std::size_t {
    return []<std::size_t... Is>(std::index_sequence<Is...>) consteval {
        std::size_t Result = 0;
        ((AcceptsFieldCount<T, Is + 1>() ? Result = Is + 1 : Result), ...);
        return Result;
    }(std::make_index_sequence<16>{});
}

export template <typename FieldT, typename FnT>
auto VisitField(FieldT& Field, FnT& On) -> void {
    // The detectable set is anything deriving from the two view records — passthrough fields (samplers,
    // bindless arrays, CPU scalars) are everything else.
    if constexpr (std::derived_from<std::remove_cvref_t<FieldT>, RGTextureViewData> ||
                  std::derived_from<std::remove_cvref_t<FieldT>, RGBufferViewData>)
        On(Field);
}

// Boilerplate generator for the structured-binding arities of ForEachView. Macros cannot be exported from a
// module, and this one must never leak to consumers.
#define RG_FOREACH_VIEW_BRANCH(ARITY, ...)                                                                             \
    if constexpr (Arity == ARITY) {                                                                                    \
        auto& [__VA_ARGS__] = Value;                                                                                   \
        std::apply([&](auto&... Field) { (VisitField(Field, On), ...); }, std::forward_as_tuple(__VA_ARGS__));         \
    }

export template <typename ParameterT, typename FnT>
auto ForEachView(ParameterT& Value, FnT&& On) -> void {
    constexpr std::size_t Arity = CountFields<std::remove_cvref_t<ParameterT>>();
    static_assert(Arity > 0, "RenderGraph Parameter must declare at least one field");
    if constexpr (Arity == 0) {
    } else
        RG_FOREACH_VIEW_BRANCH(1, M0)
    else RG_FOREACH_VIEW_BRANCH(
        2, M0, M1) else RG_FOREACH_VIEW_BRANCH(3, M0, M1, M2) else RG_FOREACH_VIEW_BRANCH(4,
                                                                                          M0,
                                                                                          M1,
                                                                                          M2,
                                                                                          M3) else RG_FOREACH_VIEW_BRANCH(5, M0, M1, M2, M3, M4) else RG_FOREACH_VIEW_BRANCH(6, M0, M1, M2, M3, M4, M5) else RG_FOREACH_VIEW_BRANCH(7, M0, M1, M2, M3, M4, M5, M6) else RG_FOREACH_VIEW_BRANCH(8, M0, M1, M2, M3, M4, M5, M6, M7) else RG_FOREACH_VIEW_BRANCH(9, M0, M1, M2, M3, M4, M5, M6, M7, M8) else RG_FOREACH_VIEW_BRANCH(10, M0, M1, M2, M3, M4, M5, M6, M7, M8, M9) else RG_FOREACH_VIEW_BRANCH(11,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         M0,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         M1,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         M2,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         M3,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         M4,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         M5,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         M6,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         M7,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         M8,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         M9,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         M10) else RG_FOREACH_VIEW_BRANCH(12,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          M0,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          M1,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          M2,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          M3,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          M4,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          M5,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          M6,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          M7,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          M8,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          M9,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          M10,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          M11) else RG_FOREACH_VIEW_BRANCH(13,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           M0,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           M1,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           M2,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           M3,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           M4,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           M5,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           M6,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           M7,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           M8,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           M9,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           M10,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           M11,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           M12) else RG_FOREACH_VIEW_BRANCH(14,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            M0,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            M1,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            M2,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            M3,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            M4,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            M5,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            M6,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            M7,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            M8,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            M9,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            M10,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            M11,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            M12,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            M13) else RG_FOREACH_VIEW_BRANCH(15,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             M0,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             M1,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             M2,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             M3,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             M4,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             M5,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             M6,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             M7,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             M8,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             M9,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             M10,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             M11,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             M12,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             M13,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             M14) else RG_FOREACH_VIEW_BRANCH(16,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              M0,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              M1,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              M2,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              M3,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              M4,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              M5,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              M6,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              M7,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              M8,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              M9,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              M10,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              M11,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              M12,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              M13,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              M14,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              M15) else {
        static_assert(Arity <= 16, "RenderGraph Parameter aggregate walk supports at most 16 fields");
    }
}

#undef RG_FOREACH_VIEW_BRANCH

export template <typename ViewT>
[[nodiscard]] auto ViewResourceIndex(const ViewT& View) -> Uint32 {
    if constexpr (requires { View.Texture; })
        return View.Texture.Index;
    else if constexpr (requires { View.Buffer; })
        return View.Buffer.Index;
    else
        static_assert(false, "RenderGraph view must expose either .Texture or .Buffer");
}

/// @brief Per-frame render graph builder (the UE FRDGBuilder analogue).
///
/// Registration (Create*/Import/AddPass) records resources and constructs passes; a single Compile()
/// validates, orders, prunes, realizes live transients, binds surviving pipelines, and assembles the
/// surviving passes into a RenderPassList. Single-use: rebuild it every frame. Pooled transient texture refs
/// return to RGTexturePool when the builder is destroyed (end of Render) — the compiled pass list holds its
/// own copies, and reuse is safe by single-queue submission ordering plus per-frame image state
/// initialization.
export class RenderGraphBuilder {
  public:
    RenderGraphBuilder()                                             = default;
    RenderGraphBuilder(const RenderGraphBuilder&)                    = delete;
    auto operator=(const RenderGraphBuilder&) -> RenderGraphBuilder& = delete;
    RenderGraphBuilder(RenderGraphBuilder&&)                         = delete;
    auto operator=(RenderGraphBuilder&&) -> RenderGraphBuilder&      = delete;

    ~RenderGraphBuilder() {
        // Return pooled transient textures immediately (RenderThread frame-end semantics). Graph-created
        // textures always come from the pool; imported resources are external and never touch the pool.
        auto& Pool = RenderGraph::Get().GetPool();
        for (auto& Resource : m_Resources) {
            if (auto* T = Resource.AsGraphTexture(); T && T->FromPool)
                Pool.Release(std::move(T->Resolved), T->Desc);
        }
    }

    /// @brief Transient buffer registration.
    ///
    /// InitialData is graph-level sugar: the graph owns a copy and realize issues CreateBuffer +
    /// UploadBuffer.
    [[nodiscard]] auto
    CreateBuffer(StringView Name, const RHIBufferDesc& Desc, std::span<const std::byte> InitialData = {}) -> RGBuffer {
        if (m_Compiled) {
            LogError("RenderGraph: CreateBuffer('{}') called after Compile(); the graph is immutable "
                     "after Compile — rebuild it each frame",
                     Name);
            return {};
        }
        if (Desc.Usage == RHIBufferUsage::Unknown) {
            LogError("RenderGraph: buffer '{}': Usage must be declared at creation — what the resource "
                     "may be used for is the creator's call; views' roles are checked against it",
                     Name);
            return {};
        }
        if (Desc.SizeBytes == 0) {
            LogError("RenderGraph: buffer '{}': SizeBytes must be non-zero", Name);
            return {};
        }
        if (!InitialData.empty() && InitialData.size_bytes() != Desc.SizeBytes) {
            LogError("RenderGraph: buffer '{}': InitialData is {} bytes but SizeBytes is {}",
                     Name,
                     InitialData.size_bytes(),
                     Desc.SizeBytes);
            return {};
        }
        // Until the graph grows a cross-frame buffer pool, a graph buffer is an RHI-transient belt slice —
        // declared by the caller, checked here.
        if (Desc.Lifetime != RHILifetime::Transient) {
            LogError("RenderGraph: buffer '{}': Lifetime must be Transient (belt slice) — graph buffer pooling "
                     "does not exist yet",
                     Name);
            return {};
        }
        // The span may point at a temporary that dies before Compile; realize uploads this owned copy
        // instead.
        std::vector<std::byte> Owned;
        if (!InitialData.empty())
            Owned.assign(InitialData.begin(), InitialData.end());
        m_Resources.emplace_back(String(Name), Desc, std::move(Owned));
        return {.Index = static_cast<uint32_t>(m_Resources.size() - 1)};
    }

    [[nodiscard]] auto CreateTexture(StringView Name, const RHITextureDesc& Desc) -> RGTexture {
        if (m_Compiled) {
            LogError("RenderGraph: CreateTexture('{}') called after Compile(); the graph is immutable after "
                     "Compile — rebuild it each frame",
                     Name);
            return {};
        }
        if (Desc.Usage == RHITextureUsage::None) {
            LogError("RenderGraph: texture '{}': Usage must be declared at creation — what the resource "
                     "may be used for is the creator's call; views' roles are checked against it",
                     Name);
            return {};
        }
        if (Desc.Width == 0 || Desc.Height == 0 || Desc.Format == RHIFormat::Unknown) {
            LogError("RenderGraph: texture '{}' has an invalid descriptor", Name);
            return {};
        }
        m_Resources.emplace_back(String(Name), Desc);
        return {.Index = static_cast<Uint32>(m_Resources.size() - 1)};
    }

    /// @brief Import an externally-owned texture.
    ///
    /// Imported resources are realized by their owner; their RHI identity is available immediately.
    [[nodiscard]] auto Import(const RHIRef<RHITexture>& Target) -> RGTexture {
        if (m_Compiled) {
            LogError("RenderGraph: Import() called after Compile(); the graph is immutable after Compile — "
                     "rebuild it each frame");
            return {};
        }
        const auto Name = Target ? String(Target->GetName()) : Format("ImportedTarget#{}", m_Resources.size());
        m_Resources.emplace_back(Name, Target);
        return {.Index = static_cast<Uint32>(m_Resources.size() - 1)};
    }

    /// @brief Import an externally-owned buffer.
    [[nodiscard]] auto Import(const RHIRef<RHIBuffer>& Buffer) -> RGBuffer {
        if (m_Compiled) {
            LogError("RenderGraph: Import() called after Compile(); the graph is immutable after Compile — "
                     "rebuild it each frame");
            return {};
        }
        const auto Name = Buffer ? String(Buffer->GetName()) : Format("ImportedBuffer#{}", m_Resources.size());
        m_Resources.emplace_back(Name, Buffer);
        return {.Index = static_cast<Uint32>(m_Resources.size() - 1)};
    }

    /// @brief Register a typed pass.
    ///
    /// The Parameter IS the resource declaration: every view-typed field contributes one access to the
    /// ordering chain; non-view fields pass through to the constructor untouched. The pass is constructed
    /// here and lives on its node from then on; a pipeline pass's pipeline is bound at Compile, when
    /// readiness is known. Rvalue-only: registration takes ownership of the Parameter. After AddPass the
    /// Parameter is frozen — wiring ran before the call, and the graph's collected view pointers assume
    /// nothing mutates it.
    template <typename TPass>
    auto AddPass(typename TPass::Parameter&& In) -> void {
        constexpr StringView PassName = TPass::Name;
        if (m_Compiled) {
            LogError("RenderGraph: AddPass('{}') called after Compile(); the graph is immutable after Compile — "
                     "rebuild it each frame",
                     PassName);
            return;
        }

        const auto PassIndex = static_cast<Uint32>(m_Passes.size());
        auto&      Node      = m_Passes.emplace_back();
        Node.Name            = String(PassName);
        if constexpr (requires { TPass::NeverPrune; })
            Node.NeverPrune = TPass::NeverPrune;
        // Stage class is known statically from the pass's RHI base type — no reflection, no runtime query.
        if constexpr (std::is_base_of_v<IRHIGraphicsPass, TPass>)
            Node.StageClass = RHIPassType::Graphics;
        else if constexpr (std::is_base_of_v<IRHIComputePass, TPass>)
            Node.StageClass = RHIPassType::Compute;
        else if constexpr (std::is_base_of_v<IRHIRayTracingPass, TPass>)
            Node.StageClass = RHIPassType::RayTracing;
        else if constexpr (std::is_base_of_v<IRHITransferPass, TPass>)
            Node.StageClass = RHIPassType::Transfer;

        if constexpr (requires { TPass::BuildPipelineRequest(); }) {
            Node.HasPipeline = true;
            Node.PipelineKey = PipelineKeyOf<TPass>();
        }

        // The pass is born here and owns the Parameter from this point on; the view pointers collected below
        // lead into that Parameter, whose address has been final since allocation.
        auto  P         = std::make_unique<typename TPass::Parameter>(std::move(In));
        auto* Parameter = P.get();
        Node.Pass       = std::make_unique<TPass>(std::move(P));

        ForEachView(*Parameter, [&](auto& Field) {
            const auto ResourceIndex = ViewResourceIndex(Field);
            if (ResourceIndex == kRGInvalidIndex || ResourceIndex >= static_cast<Uint32>(m_Resources.size())) {
                m_PendingError = Format("RenderGraph: pass '{}' touches an invalid resource handle", PassName);
                return;
            }
            // A view with no role or no access means the field was declared with the erased record type
            // instead of the role-carrying template — roles belong to the pass.
            if (Field.Usage == decltype(Field.Usage){} || Field.Access == RHIAccess::None) {
                m_PendingError =
                    Format("RenderGraph: pass '{}' has a view declaring no usage/access — declare roles in the "
                           "pass Parameter and assign identities only at the wiring site",
                           PassName);
                return;
            }

            // Last-writer chain: a writer orders after the previous writer and after every reader of the
            // previous state; a reader orders after the latest writer only. A pass never depends on itself (a
            // view pair on one resource inside one pass is that pass's own sequencing concern, not the
            // graph's).
            auto& Resource = m_Resources[ResourceIndex];
            if (magic_enum::enum_flags_test(Field.Access, RHIAccess::Write)) {
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
            if constexpr (std::derived_from<std::remove_cvref_t<decltype(Field)>, RGTextureViewData>)
                Node.Textures.push_back(&Field);
            else if constexpr (std::derived_from<std::remove_cvref_t<decltype(Field)>, RGBufferViewData>)
                Node.Buffers.push_back(&Field);
            else
                static_assert(false, "RenderGraph view field must derive from RGTextureViewData or RGBufferViewData");
        });
    }

    /// @brief Validate, order, prune, realize, and assemble the frame's passes in one call.
    ///
    /// An empty RenderPassList is a valid soft-degrade result (the frame's required pass could not be built);
    /// an unexpected is a hard frame error. Single-use: a graph compiles exactly once.
    [[nodiscard]] auto Compile() -> std::expected<RenderPassList, ErrorMessage> {
        if (m_Compiled)
            return std::unexpected(
                ErrorMessage("RenderGraph: Compile() may only be called once per graph — rebuild it each frame"));
        m_Compiled = true;
        if (!m_PendingError.empty())
            return std::unexpected(ErrorMessage(std::move(m_PendingError)));

        const auto PassCount = static_cast<Uint32>(m_Passes.size());

        // Step 1: topological order over the last-writer dependencies (repeated lowest-ready-index scan keeps
        // the order deterministic: independent passes run in registration order). Dependencies always point
        // at earlier passes, but the leftover check is the graph's only structural validation — a leftover
        // pass means the chain is broken.
        std::vector<Uint32> Order;
        Order.reserve(PassCount);
        {
            std::vector<bool> Done(PassCount, false);
            for (Uint32 Added = 0; Added < PassCount; ++Added) {
                bool Progress = false;
                for (Uint32 I = 0; I < PassCount; ++I) {
                    if (Done[I])
                        continue;
                    const bool Ready = std::ranges::all_of(m_Passes[I].Dependencies, [&](Uint32 D) { return Done[D]; });
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
                    return std::unexpected(
                        ErrorMessage(Format("RenderGraph: dependency cycle detected among passes: {}", Stuck)));
                }
            }
        }

        // Side-effect predicate, shared by Step 2 and Step 3: the pass must not be pruned (NeverPrune), or
        // writes an imported resource (the write is visible outside the graph).
        const auto IsSideEffect = [&](Uint32 I) -> bool {
            const auto& Node = m_Passes[I];
            if (Node.NeverPrune)
                return true;
            const auto WritesImported = [&](const auto& Views) {
                return std::ranges::any_of(Views, [&](const auto* V) {
                    return magic_enum::enum_flags_test(V->Access, RHIAccess::Write) &&
                           m_Resources[ViewResourceIndex(*V)].IsImported;
                });
            };
            return WritesImported(Node.Buffers) || WritesImported(Node.Textures);
        };

        // Step 2: prune. Everything a side-effect pass transitively depends on is required; every other pass
        // is dead code for this frame.
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

        // Step 3: pipeline prune. A pipeline pass whose async pipeline is not Ready yet is dropped, and every
        // required pass depending on it drops with it (its outputs never materialize). Dropping a side-effect
        // pass means the frame cannot produce its required output: return an empty list, not an error. Failed
        // pipelines are hard frame errors.
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
                return std::unexpected(ErrorMessage(
                    Format("RenderGraph: pipeline pass '{}' was never registered with the PipelineRegistry",
                           m_Passes[I].Name)));
            default: { // Pending
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

        // Step 4: realize the graph-created transients that surviving passes touch. Imported resources
        // already carry their RHI object; dead transients are never allocated. Creation usage is the caller's
        // explicit declaration; every surviving view's role is checked against it here — a pass touching a
        // resource in a way its creation did not allow is a hard frame error, not a validation-layer
        // surprise.
        std::vector<bool> Live(m_Resources.size(), false);
        // Kind is statically typed per vector — no unwrapping. The creation usage a view is checked against
        // comes from the registered desc for graph-created resources and from the realized RHI object for
        // imported ones; an import whose ref is not Ready yet skips the check (the owner validated its own
        // creation, and the prologue reports the not-ready ref at emission).
        const auto CheckBufferUsage = [&](Uint32 I, const RGBufferViewData* V) -> std::expected<void, ErrorMessage> {
            auto&       Resource = m_Resources[V->Buffer.Index];
            const auto* Prop     = std::get_if<RGBufferProp>(&Resource.Prop);
            if (!Prop)
                return {};
            RHIBufferUsage Allowed = RHIBufferUsage::Unknown;
            if (Resource.IsImported) {
                const auto* Resolved = Prop->Resolved.TryGet();
                if (!Resolved)
                    return {};
                Allowed = Resolved->GetUsage();
            } else {
                Live[V->Buffer.Index] = true;
                Allowed               = Prop->Desc.Usage;
            }
            if ((static_cast<Uint32>(V->Usage) & ~static_cast<Uint32>(Allowed)) != 0)
                return std::unexpected(
                    ErrorMessage(Format("RenderGraph: pass '{}' touches buffer '{}' in a way its creation usage does "
                                        "not allow — declare the usage union at creation",
                                        m_Passes[I].Name,
                                        Resource.Name)));
            return {};
        };
        const auto CheckTextureUsage = [&](Uint32 I, const RGTextureViewData* V) -> std::expected<void, ErrorMessage> {
            auto&       Resource = m_Resources[V->Texture.Index];
            const auto* Prop     = std::get_if<RGTextureProp>(&Resource.Prop);
            if (!Prop)
                return {};
            RHITextureUsage Allowed = RHITextureUsage::None;
            if (Resource.IsImported) {
                const auto* Resolved = Prop->Resolved.TryGet();
                if (!Resolved)
                    return {};
                Allowed = Resolved->GetUsage();
            } else {
                Live[V->Texture.Index] = true;
                Allowed                = Prop->Desc.Usage;
            }
            if ((static_cast<Uint32>(V->Usage) & ~static_cast<Uint32>(Allowed)) != 0)
                return std::unexpected(
                    ErrorMessage(Format("RenderGraph: pass '{}' touches texture '{}' in a way its creation usage does "
                                        "not allow — declare the usage union at creation",
                                        m_Passes[I].Name,
                                        Resource.Name)));
            return {};
        };
        for (Uint32 I = 0; I < PassCount; ++I) {
            if (!Required[I] || Pruned[I])
                continue;
            for (const auto* V : m_Passes[I].Buffers)
                if (auto R = CheckBufferUsage(I, V); !R)
                    return std::unexpected(std::move(R.error()));
            for (const auto* V : m_Passes[I].Textures)
                if (auto R = CheckTextureUsage(I, V); !R)
                    return std::unexpected(std::move(R.error()));
        }
        // Live is set only for graph-created resources, so any live entry needs realization.
        const bool NeedsDevice = std::ranges::any_of(Live, std::identity{});
        if (NeedsDevice) {
            // The device is touched only when a live transient exists: a graph of imported-only resources
            // never needs one. Transient textures come from the cross-frame pool; the device is hit only on a
            // pool miss.
            auto& Device = RHIRenderDevice::Get();
            auto& Pool   = RenderGraph::Get().GetPool();
            for (std::size_t R = 0; R < m_Resources.size(); ++R) {
                auto& Resource = m_Resources[R];
                if (!Live[R])
                    continue;
                if (auto* T = Resource.AsGraphTexture()) {
                    // The registered desc flows through verbatim — usage included (declared at creation).
                    auto Desc        = T->Desc;
                    Desc.MemoryUsage = RHIMemoryUsage::GpuOnly;
                    auto Created     = Pool.Acquire(Resource.Name, Desc);
                    if (!Created)
                        return std::unexpected(Created.error().Append(
                            Format("RenderGraph: failed to realize transient texture '{}'", Resource.Name)));
                    T->Resolved = std::move(*Created);
                    T->FromPool = true;
                } else if (auto* B = Resource.AsGraphBuffer()) {
                    auto Desc        = B->Desc;
                    // The graph owns placement: transient buffers are belt slices, hence CpuToGpu by
                    // construction.
                    Desc.MemoryUsage = RHIMemoryUsage::CpuToGpu;
                    auto Created     = Device.CreateBuffer(Resource.Name, Desc);
                    if (!Created)
                        return std::unexpected(Created.error().Append(
                            Format("RenderGraph: failed to realize transient buffer '{}'", Resource.Name)));
                    B->Resolved = *Created;
                    // InitialData rides the upload path: the transient slice materializes in the creation
                    // task and the upload task (same frame, FIFO) writes it.
                    if (!B->OwnedInitialData.empty())
                        if (auto R = Device.UploadBuffer(*Created, B->OwnedInitialData); !R)
                            return std::unexpected(R.error().Append(
                                Format("RenderGraph: failed to upload transient buffer '{}'", Resource.Name)));
                }
            }
        }

        // Step 5: resolve every surviving pass's views against the realized table. The pointers lead into the
        // passes' Parameters, whose addresses have been final since AddPass; this is the single write to a
        // view after registration.
        for (Uint32 I = 0; I < PassCount; ++I) {
            if (!Required[I] || Pruned[I])
                continue;
            for (auto* V : m_Passes[I].Buffers) {
                // A view's handle type already fixes the entry's kind; a null here means the wiring forged an
                // index across kinds.
                const auto* Prop = std::get_if<RGBufferProp>(&m_Resources[V->Buffer.Index].Prop);
                if (!Prop)
                    return std::unexpected(ErrorMessage(
                        Format("RenderGraph: pass '{}' views a buffer handle over a texture entry", m_Passes[I].Name)));
                V->Resolved = Prop->Resolved;
            }
            for (auto* V : m_Passes[I].Textures) {
                const auto* Prop = std::get_if<RGTextureProp>(&m_Resources[V->Texture.Index].Prop);
                if (!Prop)
                    return std::unexpected(ErrorMessage(
                        Format("RenderGraph: pass '{}' views a texture handle over a buffer entry", m_Passes[I].Name)));
                V->Resolved = Prop->Resolved;
            }
        }

        // Step 6: bind the surviving pipeline passes' Ready pipelines, then move every survivor into the
        // frame list in final order, each paired with its prologue — the dst-only state requirements its
        // declared accesses map to.
        RenderPassList Result{};
        auto&          Registry = PipelineRegistry::Get();
        for (const auto I : Order) {
            if (!Required[I] || Pruned[I])
                continue;
            auto& Node = m_Passes[I];
            if (Node.HasPipeline) {
                // Prune already saw Ready; the registry answers again here, and a pipeline gone in between is
                // a hard frame error.
                const auto Bind = [&](auto* Pass, auto Pipeline) -> std::expected<void, ErrorMessage> {
                    if (!Pipeline)
                        return std::unexpected(ErrorMessage(
                            Format("RenderGraph: pipeline for pass '{}' was Ready at prune time but is gone at "
                                   "bind",
                                   Node.Name)));
                    Pass->BindPipeline(std::move(Pipeline));
                    return {};
                };
                std::expected<void, ErrorMessage> Bound;
                switch (Node.StageClass) {
                case RHIPassType::Graphics:
                    Bound = Bind(static_cast<IRHIGraphicsPass*>(Node.Pass.get()),
                                 Registry.GetReadyGraphics(Node.PipelineKey));
                    break;
                case RHIPassType::Compute:
                    Bound = Bind(static_cast<IRHIComputePass*>(Node.Pass.get()),
                                 Registry.GetReadyCompute(Node.PipelineKey));
                    break;
                case RHIPassType::RayTracing:
                    Bound = Bind(static_cast<IRHIRayTracingPass*>(Node.Pass.get()),
                                 Registry.GetReadyRayTracing(Node.PipelineKey));
                    break;
                default:
                    break;
                }
                if (!Bound)
                    return std::unexpected(std::move(Bound.error()));
            }
            Result.Entries.push_back({.Pass = std::move(Node.Pass), .Prologue = BuildPassPrologue(Node, m_Resources)});
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
