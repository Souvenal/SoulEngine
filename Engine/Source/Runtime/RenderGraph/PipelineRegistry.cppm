module;

#include <entt/entt.hpp>

export module RenderGraph:PipelineRegistry;

export import Core;
import RHI;
import ShaderCompiler;
import TaskGraph;
export import std;

export namespace SoulEngine {

/// @brief Runtime state of one registered pipeline key (facing the renderer / Compile).
enum class RGPipelineState : Uint8 {
    Unknown = 0,   ///< Not registered.
    Pending,       ///< Registered but not started, or async started and not yet Ready.
    Ready,
    Failed,
};

/// @brief Async graphics pipeline request descriptor (moved from Resource:Types).
struct GraphicsPipelineRequest {
    ShaderEntry              VertEntry         = {};
    ShaderEntry              FragEntry         = {};
    RHIVertexInputLayoutDesc VertexInputLayout = {};
    RHIPrimitiveTopology     Topology          = RHIPrimitiveTopology::TriangleList;
    RHIRasterizerState       Rasterizer        = {};
    RHIBlendState            Blend             = {};
    RHIDepthStencilState     DepthStencil      = {};
    std::vector<RHIFormat>   ColorFormats      = {RHIFormat::B8G8R8A8_UNORM};
    RHIFormat                DepthFormat       = RHIFormat::Unknown;
};

/// @brief Async compute pipeline request descriptor (moved from Resource:Types).
struct ComputePipelineRequest {
    ShaderEntry ComputeEntry = {};
};

/// @brief Async ray-tracing pipeline request descriptor (moved from
/// Resource:Types; the Resource pipeline workflow is retired).
struct RayTracingPipelineRequest {
    ShaderEntry                                RayGeneration     = {};
    std::vector<ShaderEntry>                   MissEntries       = {};
    std::vector<RayTracingHitGroupCompileDesc> HitGroups         = {};
    Uint32                                     MaxRecursionDepth = 1;
};

} // namespace SoulEngine

namespace SoulEngine {
namespace {

/// Idempotence predicate for Register: same key + same descriptor is a no-op.
/// Requests compare field by field (ShaderEntry paths and entry points are the
/// meaningful identity; pipeline fixed-function state uses defaulted equality).
[[nodiscard]] auto SameGraphicsRequest(const GraphicsPipelineRequest& A, const GraphicsPipelineRequest& B) -> bool {
    return A.VertEntry.SourcePath == B.VertEntry.SourcePath && A.VertEntry.EntryPoint == B.VertEntry.EntryPoint &&
           A.FragEntry.SourcePath == B.FragEntry.SourcePath && A.FragEntry.EntryPoint == B.FragEntry.EntryPoint &&
           A.ColorFormats == B.ColorFormats && A.DepthFormat == B.DepthFormat;
}

[[nodiscard]] auto SameComputeRequest(const ComputePipelineRequest& A, const ComputePipelineRequest& B) -> bool {
    return A.ComputeEntry.SourcePath == B.ComputeEntry.SourcePath &&
           A.ComputeEntry.EntryPoint == B.ComputeEntry.EntryPoint;
}

[[nodiscard]] auto SameEntryList(const std::vector<ShaderEntry>& A, const std::vector<ShaderEntry>& B) -> bool {
    return A.size() == B.size() &&
           std::ranges::equal(A, B, [](const ShaderEntry& Lhs, const ShaderEntry& Rhs) {
               return Lhs.SourcePath == Rhs.SourcePath && Lhs.EntryPoint == Rhs.EntryPoint;
           });
}

[[nodiscard]] auto SameRayTracingRequest(const RayTracingPipelineRequest& A, const RayTracingPipelineRequest& B)
    -> bool {
    const auto SameHitGroups = [](const std::vector<RayTracingHitGroupCompileDesc>& Lhs,
                                  const std::vector<RayTracingHitGroupCompileDesc>& Rhs) {
        const auto SameOptionalEntry = [](const std::optional<ShaderEntry>& L, const std::optional<ShaderEntry>& R) {
            if (L.has_value() != R.has_value())
                return false;
            return !L || (L->SourcePath == R->SourcePath && L->EntryPoint == R->EntryPoint);
        };
        return Lhs.size() == Rhs.size() &&
               std::ranges::equal(Lhs, Rhs, [&](const RayTracingHitGroupCompileDesc& L,
                                                const RayTracingHitGroupCompileDesc& R) {
                   return L.Type == R.Type && SameOptionalEntry(L.ClosestHit, R.ClosestHit) &&
                          SameOptionalEntry(L.AnyHit, R.AnyHit) && SameOptionalEntry(L.Intersection, R.Intersection);
               });
    };
    return A.RayGeneration.SourcePath == B.RayGeneration.SourcePath &&
           A.RayGeneration.EntryPoint == B.RayGeneration.EntryPoint &&
           SameEntryList(A.MissEntries, B.MissEntries) && SameHitGroups(A.HitGroups, B.HitGroups) &&
           A.MaxRecursionDepth == B.MaxRecursionDepth;
}

/// Map an async RHIRef state onto the registry-facing pipeline state.
[[nodiscard]] auto MapRefState(RHIRefState State) -> RGPipelineState {
    switch (State) {
    case RHIRefState::Ready:
        return RGPipelineState::Ready;
    case RHIRefState::Failed:
        return RGPipelineState::Failed;
    default:
        return RGPipelineState::Pending;
    }
}

/// Shared RHI-thread tail for graphics/compute async creation: build the
/// binding set, publish it, create the pipeline, publish it. `Create` is the
/// only pipeline-kind-specific step.
template <typename PipelineT, typename PipelineDescT, typename CreateFn>
auto PublishPipelineOnRhiThread(RHIRef<PipelineT> PipelineRef,
                                RHIRef<RHIShaderBindingSet> BindingSetRef,
                                String Name,
                                PipelineDescT PipelineDesc,
                                ShaderStage Stages,
                                CreateFn&& Create) -> void {
    const auto BindingSetDesc = RHIShaderBindingSetDesc{
        .Reflection = PipelineDesc.Program.Reflection,
        .Stages     = Stages,
    };
    auto BindingSet = RHIRenderDevice::Get().CreateShaderBindingSet(Name, BindingSetDesc);
    if (!BindingSet) {
        BindingSetRef.MarkFailed(BindingSet.error());
        PipelineRef.MarkFailed(BindingSet.error());
        return;
    }
    if (auto Publish = BindingSetRef.Publish(std::move(*BindingSet), RHIRefState::Ready); !Publish) {
        BindingSetRef.MarkFailed(Publish.error());
        PipelineRef.MarkFailed(Publish.error());
        return;
    }
    PipelineDesc.BindingSet = BindingSetRef;
    auto Created = std::forward<CreateFn>(Create)(Name, PipelineDesc);
    if (!Created) {
        PipelineRef.MarkFailed(Created.error());
        return;
    }
    if (auto Publish = PipelineRef.Publish(std::move(*Created), RHIRefState::Ready); !Publish)
        PipelineRef.MarkFailed(Publish.error());
}

} // namespace
} // namespace SoulEngine

export namespace SoulEngine {

/// Registry key for a typed pipeline pass: the entt type hash, so the graph
/// (type-erased after AddPass) and the renderer's typed Register<TPass> agree
/// on the key without sharing a string convention.
template <typename TPass>
[[nodiscard]] inline auto PipelineKeyOf() -> String {
    return Format("{}", entt::type_hash<TPass>::value());
}

/// @brief Engine-level, cross-frame pipeline registry. Key = pass type hash.
///
/// Lifecycle is owned by Launch: `Init()` runs before SelectRenderer
/// (renderer OnAttach will Register); `Clear()` must run before
/// `RHIRenderDevice::Destroy()`. Per-frame RenderGraph objects never hold
/// cross-frame pipelines: Compile resolves Ready refs via
/// `GetReadyGraphics` / `GetReadyCompute`.
///
/// Threading: Register may run on the main thread (OnAttach, including the
/// re-OnAttach path from a runtime SelectRenderer switch) and on the Render
/// thread; EnsureRequestStarted / Get* run on the Render thread; the internal
/// map is mutex-protected. Async completion writes RHIRefPayload atomics.
class PipelineRegistry final : public Singleton<PipelineRegistry> {
    friend class Singleton<PipelineRegistry>;

  public:
    // ── Lifecycle (Launch calls these explicitly) ───────────────────────

    /// Clear and reset (idempotent; call before SelectRenderer).
    auto Init() -> void {
        std::scoped_lock Lock(m_Mutex);
        m_Entries.clear();
    }

    /// Release every entry (pipeline RHIRefs drop to empty). Must run before
    /// `RHIRenderDevice::Destroy()` — payload destruction needs a live queue.
    auto Clear() -> void {
        std::scoped_lock Lock(m_Mutex);
        // Dropping every entry releases the last RHIRef on each pipeline and
        // enqueues payload destruction onto GDeferredDeletionQueue
        // (RHIRef.cppm:89-95). The caller (Launch Shutdown) must keep Clear()
        // ahead of RHIRenderDevice::Destroy() (D4).
        m_Entries.clear();
    }

    // ── Descriptor registration (renderer OnAttach; no async, no refs) ──

    /// Typed registration: pulls the descriptor from `TPass::BuildPipelineRequest()`
    /// and keys it by the pass type. The request's type selects graphics,
    /// compute, or ray tracing.
    template <typename TPass>
    auto Register() -> void {
        if constexpr (std::same_as<decltype(TPass::BuildPipelineRequest()), GraphicsPipelineRequest>)
            RegisterGraphics(PipelineKeyOf<TPass>(), TPass::BuildPipelineRequest());
        else if constexpr (std::same_as<decltype(TPass::BuildPipelineRequest()), ComputePipelineRequest>)
            RegisterCompute(PipelineKeyOf<TPass>(), TPass::BuildPipelineRequest());
        else
            RegisterRayTracing(PipelineKeyOf<TPass>(), TPass::BuildPipelineRequest());
    }

    auto RegisterGraphics(StringView Key, const GraphicsPipelineRequest& Desc) -> void {
        Register(Key, RHIPassType::Graphics, Desc, &Entry::GraphicsDesc, SameGraphicsRequest);
    }

    auto RegisterCompute(StringView Key, const ComputePipelineRequest& Desc) -> void {
        Register(Key, RHIPassType::Compute, Desc, &Entry::ComputeDesc, SameComputeRequest);
    }

    auto RegisterRayTracing(StringView Key, const RayTracingPipelineRequest& Desc) -> void {
        Register(Key, RHIPassType::RayTracing, Desc, &Entry::RayTracingDesc, SameRayTracingRequest);
    }

    // ── Lazy start + query (idempotent; D5: first frame that needs the Pass) ─

    /// Start once if not yet started; later calls are no-ops.
    auto EnsureRequestStarted(StringView Key) -> void {
        std::scoped_lock Lock(m_Mutex);
        const auto       It = m_Entries.find(String(Key));
        if (It == m_Entries.end())
            return;  // Unregistered: no-op; AddPipelinePass / renderer snapshot report the config error.
        auto& E = It->second;
        if (E.Requested)
            return;  // Idempotent: start exactly once (D5).
        E.Requested = true;
        const auto Name = String(Key);
        switch (E.Kind) {
        case RHIPassType::Graphics:
            StartGraphicsAsyncLocked(E, Name);
            break;
        case RHIPassType::Compute:
            StartComputeAsyncLocked(E, Name);
            break;
        case RHIPassType::RayTracing:
            StartRayTracingAsyncLocked(E, Name);
            break;
        default:
            break;
        }
    }

    [[nodiscard]] auto IsRegistered(StringView Key) const -> bool {
        std::scoped_lock Lock(m_Mutex);
        return m_Entries.contains(String(Key));
    }

    [[nodiscard]] auto GetState(StringView Key) const -> RGPipelineState {
        std::scoped_lock Lock(m_Mutex);
        const auto       It = m_Entries.find(String(Key));
        if (It == m_Entries.end())
            return RGPipelineState::Unknown;
        const auto& E = It->second;
        if (!E.Requested)
            return RGPipelineState::Pending;  // Registered but not started (§4.1).
        switch (E.Kind) {
        case RHIPassType::Graphics:
            return MapRefState(E.GraphicsRef.GetState());
        case RHIPassType::Compute:
            return MapRefState(E.ComputeRef.GetState());
        case RHIPassType::RayTracing:
            return MapRefState(E.RayTracingRef.GetState());
        default:
            return RGPipelineState::Pending;
        }
    }

    [[nodiscard]] auto GetError(StringView Key) const -> std::optional<ErrorMessage> {
        std::scoped_lock Lock(m_Mutex);
        const auto       It = m_Entries.find(String(Key));
        if (It == m_Entries.end())
            return std::nullopt;
        const auto& E = It->second;
        switch (E.Kind) {
        case RHIPassType::Graphics:
            return E.GraphicsRef.GetError();
        case RHIPassType::Compute:
            return E.ComputeRef.GetError();
        case RHIPassType::RayTracing:
            return E.RayTracingRef.GetError();
        default:
            return std::nullopt;
        }
    }

    // ── Resolve Ready refs inside Record callbacks (open item ②) ────────
    // Returns a shared-ref copy when Ready; empty when not Ready / unregistered.
    // The entry keeps owning the payload, so use inside Execute() is safe.

    [[nodiscard]] auto GetReadyGraphics(StringView Key) const -> RHIRef<RHIGraphicsPipeline> {
        return GetReady(Key, RHIPassType::Graphics, &Entry::GraphicsRef);
    }

    [[nodiscard]] auto GetReadyCompute(StringView Key) const -> RHIRef<RHIComputePipeline> {
        return GetReady(Key, RHIPassType::Compute, &Entry::ComputeRef);
    }

    [[nodiscard]] auto GetReadyRayTracing(StringView Key) const -> RHIRef<RHIRayTracingPipeline> {
        return GetReady(Key, RHIPassType::RayTracing, &Entry::RayTracingRef);
    }

  private:
    PipelineRegistry()  = default;
    ~PipelineRegistry() = default;

    struct Entry {
        bool                        Requested    = false;  // EnsureRequestStarted has been called.
        RHIPassType                 Kind         = RHIPassType::Unknown;
        GraphicsPipelineRequest     GraphicsDesc = {};
        ComputePipelineRequest      ComputeDesc  = {};
        RayTracingPipelineRequest   RayTracingDesc = {};
        RHIRef<RHIGraphicsPipeline> GraphicsRef  = nullptr;
        RHIRef<RHIComputePipeline>  ComputeRef   = nullptr;
        RHIRef<RHIRayTracingPipeline> RayTracingRef = nullptr;
    };

    mutable std::mutex                m_Mutex   = {};
    std::unordered_map<String, Entry> m_Entries = {};

    /// Shared RegisterGraphics / RegisterCompute body. Idempotent on same key +
    /// same descriptor; on conflict, LogWarning and keep the first registration
    /// (t9 repair option: later may return expected<void,ErrorMessage> or emit a
    /// LogError with a descriptor diff for RasterRenderer::OnAttach — this pass
    /// keeps current semantics per review).
    template <typename DescT, typename SamePred>
    auto Register(StringView Key,
                  RHIPassType Kind,
                  const DescT& Desc,
                  DescT Entry::*DescMember,
                  SamePred&& Same) -> void {
        std::scoped_lock Lock(m_Mutex);
        const auto       KeyString = String(Key);
        auto [It, Inserted]        = m_Entries.try_emplace(KeyString);
        auto& E                    = It->second;
        if (!Inserted) {
            if (E.Kind == Kind && Same(E.*DescMember, Desc))
                return;  // Idempotent: same key + same descriptor ⇒ no-op.
            LogWarning("PipelineRegistry: Register('{}') ignored: the key already holds a different pipeline "
                       "registration (keeping the first)",
                       Key);
            return;
        }
        E.Kind        = Kind;
        E.*DescMember = Desc;
    }

    template <typename PipelineT>
    [[nodiscard]] auto GetReady(StringView Key,
                                RHIPassType Kind,
                                RHIRef<PipelineT> Entry::*RefMember) const -> RHIRef<PipelineT> {
        std::scoped_lock Lock(m_Mutex);
        const auto       It = m_Entries.find(String(Key));
        if (It == m_Entries.end())
            return nullptr;
        const auto& E = It->second;
        if (E.Kind != Kind || (E.*RefMember).GetState() != RHIRefState::Ready)
            return nullptr;
        return E.*RefMember;  // Shared-payload copy; entry keeps ownership, safe in Execute().
    }

    /// Async create (moved from ResourcePipeline; logic equivalent).
    auto StartGraphicsAsyncLocked(Entry& E, String Name) -> void {
        auto PipelineRef   = RHIRef<RHIGraphicsPipeline>::Create();
        auto BindingSetRef = RHIRef<RHIShaderBindingSet>::Create();
        E.GraphicsRef      = PipelineRef;  // Entry keeps the ref so GetState / GetReady can observe it.

        // Capture the descriptor by value into the background closure; the entry
        // itself only stores the ref.
        auto Desc          = E.GraphicsDesc;
        auto EnqueueResult = TaskGraph::Get().EnqueueBackground(
            [Desc = std::move(Desc), Name = std::move(Name), PipelineRef, BindingSetRef]() mutable {
                const auto&       Cfg = ConfigManager::Get();
                std::vector<Path> IncludeDirs{Cfg.EngineShadersDirPath()};

                auto Program = ShaderCompiler::Get().CompileGraphics(GraphicsCompileDesc{
                    .Vertex      = Desc.VertEntry,
                    .Fragment    = Desc.FragEntry,
                    .IncludeDirs = IncludeDirs,
                });
                if (!Program) {
                    auto Error = Program.error().Append(Format("Graphics pipeline shaders '{}'/'{}' + '{}'/'{}'",
                                                               Desc.VertEntry.SourcePath.string(),
                                                               Desc.VertEntry.EntryPoint,
                                                               Desc.FragEntry.SourcePath.string(),
                                                               Desc.FragEntry.EntryPoint));
                    LogError("Failed to prepare graphics pipeline: {}", Error.ToString());
                    PipelineRef.MarkFailed(std::move(Error));
                    return;
                }

                auto PipelineDesc = RHIGraphicsPipelineDesc{
                    .Program           = std::move(*Program),
                    .VertexInputLayout = Desc.VertexInputLayout,
                    .Topology          = Desc.Topology,
                    .Rasterizer        = Desc.Rasterizer,
                    .Blend             = Desc.Blend,
                    .DepthStencil      = Desc.DepthStencil,
                    .ColorFormats      = Desc.ColorFormats,
                    .DepthFormat       = Desc.DepthFormat,
                };
                auto EnqueueResult = TaskGraph::Get().EnqueueTask(
                    ThreadQueue::RHI,
                    [PipelineRef, BindingSetRef, Name = std::move(Name),
                     PipelineDesc = std::move(PipelineDesc)]() mutable {
                        using magic_enum::bitwise_operators::operator|;
                        PublishPipelineOnRhiThread(
                            PipelineRef,
                            BindingSetRef,
                            std::move(Name),
                            std::move(PipelineDesc),
                            ShaderStage::Vertex | ShaderStage::Fragment,
                            [](const String& PipelineName, const RHIGraphicsPipelineDesc& PipelineDescArg) {
                                return RHIRenderDevice::Get().CreateGraphicsPipeline(PipelineName, PipelineDescArg);
                            });
                    });
                if (!EnqueueResult)
                    PipelineRef.MarkFailed(EnqueueResult.error());
            });
        if (!EnqueueResult)
            PipelineRef.MarkFailed(EnqueueResult.error());
    }

    auto StartComputeAsyncLocked(Entry& E, String Name) -> void {
        auto PipelineRef   = RHIRef<RHIComputePipeline>::Create();
        auto BindingSetRef = RHIRef<RHIShaderBindingSet>::Create();
        E.ComputeRef       = PipelineRef;

        auto Desc          = E.ComputeDesc;
        auto EnqueueResult = TaskGraph::Get().EnqueueBackground(
            [Desc = std::move(Desc), Name = std::move(Name), PipelineRef, BindingSetRef]() mutable {
                const auto&       Cfg = ConfigManager::Get();
                std::vector<Path> IncludeDirs{Cfg.EngineShadersDirPath()};

                auto Program = ShaderCompiler::Get().CompileCompute(ComputeCompileDesc{
                    .Compute     = Desc.ComputeEntry,
                    .IncludeDirs = IncludeDirs,
                });
                if (!Program) {
                    auto Error = Program.error().Append(Format("Compute pipeline shader '{}'/'{}'",
                                                               Desc.ComputeEntry.SourcePath.string(),
                                                               Desc.ComputeEntry.EntryPoint));
                    LogError("Failed to prepare compute pipeline: {}", Error.ToString());
                    PipelineRef.MarkFailed(std::move(Error));
                    return;
                }

                auto PipelineDesc = RHIComputePipelineDesc{
                    .Program = std::move(*Program),
                };
                auto EnqueueResult = TaskGraph::Get().EnqueueTask(
                    ThreadQueue::RHI,
                    [PipelineRef, BindingSetRef, Name = std::move(Name),
                     PipelineDesc = std::move(PipelineDesc)]() mutable {
                        PublishPipelineOnRhiThread(
                            PipelineRef,
                            BindingSetRef,
                            std::move(Name),
                            std::move(PipelineDesc),
                            ShaderStage::Compute,
                            [](const String& PipelineName, const RHIComputePipelineDesc& PipelineDescArg) {
                                return RHIRenderDevice::Get().CreateComputePipeline(PipelineName, PipelineDescArg);
                            });
                    });
                if (!EnqueueResult)
                    PipelineRef.MarkFailed(EnqueueResult.error());
            });
        if (!EnqueueResult)
            PipelineRef.MarkFailed(EnqueueResult.error());
    }

    /// Async create (logic moved from the retired Resource:Pipeline workflow).
    auto StartRayTracingAsyncLocked(Entry& E, String Name) -> void {
        auto PipelineRef   = RHIRef<RHIRayTracingPipeline>::Create();
        auto BindingSetRef = RHIRef<RHIShaderBindingSet>::Create();
        E.RayTracingRef    = PipelineRef;

        auto Desc          = E.RayTracingDesc;
        auto EnqueueResult = TaskGraph::Get().EnqueueBackground(
            [Desc = std::move(Desc), Name = std::move(Name), PipelineRef, BindingSetRef]() mutable {
                const auto&       Cfg = ConfigManager::Get();
                std::vector<Path> IncludeDirs{Cfg.EngineShadersDirPath()};

                auto Program = ShaderCompiler::Get().CompileRayTracing(RayTracingCompileDesc{
                    .RayGeneration = Desc.RayGeneration,
                    .MissEntries   = Desc.MissEntries,
                    .HitGroups     = Desc.HitGroups,
                    .IncludeDirs   = IncludeDirs,
                });
                if (!Program) {
                    auto Error = Program.error().Append(Format(
                        "Ray-tracing pipeline shader '{}'", Desc.RayGeneration.SourcePath.string()));
                    LogError("Failed to prepare ray-tracing pipeline: {}", Error.ToString());
                    PipelineRef.MarkFailed(std::move(Error));
                    return;
                }

                auto PipelineDesc = RHIRayTracingPipelineDesc{
                    .Program           = std::move(*Program),
                    .MaxRecursionDepth = Desc.MaxRecursionDepth,
                };
                auto EnqueueResult = TaskGraph::Get().EnqueueTask(
                    ThreadQueue::RHI,
                    [PipelineRef, BindingSetRef, Name = std::move(Name),
                     PipelineDesc = std::move(PipelineDesc)]() mutable {
                        using magic_enum::bitwise_operators::operator|;
                        PublishPipelineOnRhiThread(
                            PipelineRef,
                            BindingSetRef,
                            std::move(Name),
                            std::move(PipelineDesc),
                            ShaderStage::RayGeneration | ShaderStage::Intersection | ShaderStage::AnyHit |
                                ShaderStage::ClosestHit | ShaderStage::Miss | ShaderStage::Callable,
                            [](const String& PipelineName, const RHIRayTracingPipelineDesc& PipelineDescArg) {
                                return RHIRenderDevice::Get().CreateRayTracingPipeline(PipelineName,
                                                                                       PipelineDescArg);
                            });
                    });
                if (!EnqueueResult)
                    PipelineRef.MarkFailed(EnqueueResult.error());
            });
        if (!EnqueueResult)
            PipelineRef.MarkFailed(EnqueueResult.error());
    }
};

} // namespace SoulEngine
