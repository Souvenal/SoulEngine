export module RenderGraph.PipelineRegistry;

import Core;
import RHI;
import ShaderCompiler;
import TaskGraph;
import std;
import Shader;

namespace SoulEngine {

/// @brief Runtime state of one registered pipeline (facing the renderer / Compile).
export enum class RGPipelineState : Uint8 {
    Unknown = 0, ///< Not registered.
    Pending,     ///< Registered but not started, or async started and not yet Ready.
    Ready,
    Failed,
};

/// @brief Async graphics pipeline request descriptor (moved from Resource:Types).
export struct GraphicsPipelineRequest {
    ShaderEntry                          VertEntry         = {};
    ShaderEntry                          FragEntry         = {};
    RHIVertexInputLayoutDesc             VertexInputLayout = {};
    RHIPrimitiveTopology                 Topology          = RHIPrimitiveTopology::TriangleList;
    RHIRasterizerState                   Rasterizer        = {};
    RHIDepthStencilState                 DepthStencil      = {};
    std::vector<RHIColorAttachmentState> ColorAttachments  = {};
    RHIFormat                            DepthFormat       = RHIFormat::Unknown;

    bool operator==(const GraphicsPipelineRequest&) const = default;
};

/// @brief Async compute pipeline request descriptor (moved from Resource:Types).
export struct ComputePipelineRequest {
    ShaderEntry                            ComputeEntry            = {};
    /// @brief Specialization constant values fixed at pipeline creation.
    ///
    /// They are part of the pipeline identity: two value sets must produce two pipelines.
    std::vector<RHISpecializationConstant> SpecializationConstants = {};

    bool operator==(const ComputePipelineRequest&) const = default;
};

/// @brief Async ray-tracing pipeline request descriptor (moved from
/// Resource:Types; the Resource pipeline workflow is retired).
export struct RayTracingPipelineRequest {
    ShaderEntry                                RayGeneration     = {};
    std::vector<ShaderEntry>                   MissEntries       = {};
    std::vector<RayTracingHitGroupCompileDesc> HitGroups         = {};
    Uint32                                     MaxRecursionDepth = 1;

    bool operator==(const RayTracingPipelineRequest&) const = default;
};

/// @brief One of the three async pipeline request descriptors.
export template <typename T>
concept PipelineRequest = std::same_as<T, GraphicsPipelineRequest> || std::same_as<T, ComputePipelineRequest> ||
                          std::same_as<T, RayTracingPipelineRequest>;

/// @brief A pass type that declares a static pipeline request under a type-level name.
///
/// RenderGraphBuilder::AddPass registers (TPass::Name, TPass::BuildPipelineRequest()) when this concept
/// holds; pass kinds without a pipeline request (e.g. transfer passes) skip registration.
/// BuildPipelineRequest() must return the same descriptor on every call: AddPass re-registers each frame,
/// and a changed descriptor under an existing name is rejected as a conflicting registration.
export template <typename TPass>
concept PipelineRequestProvider = requires {
    { TPass::Name } -> std::convertible_to<StringView>;
    { TPass::BuildPipelineRequest() } -> PipelineRequest;
};

/// @brief Engine-level, cross-frame pipeline registry keyed by pass name.
///
/// Lifecycle is owned by Launch: `Init()` runs before SelectRenderer; `Clear()` must run before
/// `RHIRenderDevice::Destroy()`. Registration is lazy: the first AddPass of each pipeline-request pass
/// type registers its descriptor under the type-level name, so pass types a frame never uses hold no
/// entry. Per-frame RenderGraph objects never hold cross-frame pipelines: Compile resolves Ready refs via
/// `GetReadyGraphics` / `GetReadyCompute`.
///
/// Threading: Register runs inside AddPass on the render thread and is idempotent across frames;
/// EnsureRequestStarted / Get* run on the Render thread; the internal map is mutex-protected. Async
/// completion writes RHIRefPayload atomics.
export class PipelineRegistry final : public Singleton<PipelineRegistry> {
    friend class Singleton<PipelineRegistry>;

  public:
    // Lifecycle (Launch calls these explicitly)

    /// Clear and reset (idempotent; call before SelectRenderer).
    auto Init() -> void;

    /// @brief Release every entry (pipeline RHIRefs drop to empty).
    ///
    /// Must run before `RHIRenderDevice::Destroy()` — payload destruction needs a live queue.
    auto Clear() -> void;

    // Descriptor registration (no async, no refs; idempotent from AddPass each frame)

    /// @brief Register a pipeline request under its pass name.
    ///
    /// Re-registering the same name with the same descriptor is a no-op. A different descriptor under an
    /// existing name is a fatal registration: the first registration keeps the name, and the rejected
    /// pipeline stays unregistered so its pass reports a configuration error.
    auto Register(StringView Name, const GraphicsPipelineRequest& Desc) -> void;
    auto Register(StringView Name, const ComputePipelineRequest& Desc) -> void;
    auto Register(StringView Name, const RayTracingPipelineRequest& Desc) -> void;

    // Lazy start + query (idempotent; first frame that needs the pass starts the async compile)

    /// Start once if not yet started; later calls are no-ops.
    auto EnsureRequestStarted(StringView Name) -> void;

    [[nodiscard]] auto IsRegistered(StringView Name) const -> bool;

    [[nodiscard]] auto GetState(StringView Name) const -> RGPipelineState;

    [[nodiscard]] auto GetError(StringView Name) const -> std::optional<ErrorMessage>;

    // Resolve Ready refs inside Record callbacks.
    // Returns a shared-ref copy when Ready; empty when not Ready / unregistered. The entry keeps owning the
    // payload, so use inside Execute() is safe.

    [[nodiscard]] auto GetReadyGraphics(StringView Name) const -> RHIRef<RHIGraphicsPipeline>;

    [[nodiscard]] auto GetReadyCompute(StringView Name) const -> RHIRef<RHIComputePipeline>;

    [[nodiscard]] auto GetReadyRayTracing(StringView Name) const -> RHIRef<RHIRayTracingPipeline>;

  private:
    PipelineRegistry()  = default;
    ~PipelineRegistry() = default;

    struct Entry {
        bool                          Requested      = false; // EnsureRequestStarted has been called.
        RHIPassType                   Kind           = RHIPassType::Unknown;
        GraphicsPipelineRequest       GraphicsDesc   = {};
        ComputePipelineRequest        ComputeDesc    = {};
        RayTracingPipelineRequest     RayTracingDesc = {};
        RHIRef<RHIGraphicsPipeline>   GraphicsRef    = nullptr;
        RHIRef<RHIComputePipeline>    ComputeRef     = nullptr;
        RHIRef<RHIRayTracingPipeline> RayTracingRef  = nullptr;
    };

    mutable std::mutex                m_Mutex   = {};
    std::unordered_map<String, Entry> m_Entries = {};

    /// @brief Shared Register body for the three request overloads.
    template <typename DescT>
    auto RegisterImpl(StringView Name, RHIPassType Kind, const DescT& Desc, DescT Entry::* DescMember) -> void {
        std::scoped_lock Lock(m_Mutex);
        const auto       NameString = String(Name);
        auto [It, Inserted]         = m_Entries.try_emplace(NameString);
        auto& E                     = It->second;
        if (!Inserted) {
            if (E.Kind == Kind && E.*DescMember == Desc)
                return; // Idempotent: same name + same descriptor, re-registered by every frame's AddPass.
            LogError("PipelineRegistry: Register('{}') rejected: the name already holds a different pipeline "
                     "registration (keeping the first)",
                     Name);
            return;
        }
        E.Kind        = Kind;
        E.*DescMember = Desc;
    }

    template <typename PipelineT>
    [[nodiscard]] auto GetReady(StringView Name, RHIPassType Kind, RHIRef<PipelineT> Entry::* RefMember) const
        -> RHIRef<PipelineT> {
        std::scoped_lock Lock(m_Mutex);
        const auto       It = m_Entries.find(String(Name));
        if (It == m_Entries.end())
            return nullptr;
        const auto& E = It->second;
        if (E.Kind != Kind || (E.*RefMember).GetState() != RHIRefState::Ready)
            return nullptr;
        return E.*RefMember; // Shared-payload copy; entry keeps ownership, safe in Execute().
    }

    /// Async create; each starts the shader compile on a background queue and publishes on the RHI queue.
    auto StartGraphicsAsyncLocked(Entry& E, String Name) -> void;
    auto StartComputeAsyncLocked(Entry& E, String Name) -> void;
    auto StartRayTracingAsyncLocked(Entry& E, String Name) -> void;
};

/// Map an async RHIRef state onto the registry-facing pipeline state.
export [[nodiscard]] auto MapRefState(RHIRefState State) -> RGPipelineState;

/// @brief Shared RHI-thread tail for graphics/compute async creation.
///
/// Build the binding set, publish it, create the pipeline, publish it. `Create` is the only
/// pipeline-kind-specific step.
export template <typename PipelineT, typename PipelineDescT, typename CreateFn>
auto PublishPipelineOnRhiThread(RHIRef<PipelineT>           PipelineRef,
                                RHIRef<RHIShaderBindingSet> BindingSetRef,
                                String                      Name,
                                PipelineDescT               PipelineDesc,
                                ShaderStage                 Stages,
                                CreateFn&&                  Create) -> void {
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
    auto Created            = std::forward<CreateFn>(Create)(Name, PipelineDesc);
    if (!Created) {
        PipelineRef.MarkFailed(Created.error());
        return;
    }
    if (auto Publish = PipelineRef.Publish(std::move(*Created), RHIRefState::Ready); !Publish)
        PipelineRef.MarkFailed(Publish.error());
}

} // namespace SoulEngine
