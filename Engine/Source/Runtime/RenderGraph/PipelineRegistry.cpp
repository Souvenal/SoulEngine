module RenderGraph.PipelineRegistry;

import Core;
import RHI;
import ShaderCompiler;
import TaskGraph;
import std;
import Shader;

namespace SoulEngine {

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

auto PipelineRegistry::Init() -> void {
    std::scoped_lock Lock(m_Mutex);
    m_Entries.clear();
}

auto PipelineRegistry::Clear() -> void {
    std::scoped_lock Lock(m_Mutex);
    // Dropping every entry releases the last RHIRef on each pipeline and enqueues payload destruction
    // onto GDeferredDeletionQueue; the caller must keep Clear() ahead of RHIRenderDevice::Destroy().
    m_Entries.clear();
}

auto PipelineRegistry::Register(StringView Name, const GraphicsPipelineRequest& Desc) -> void {
    RegisterImpl(Name, RHIPassType::Graphics, Desc, &Entry::GraphicsDesc);
}

auto PipelineRegistry::Register(StringView Name, const ComputePipelineRequest& Desc) -> void {
    RegisterImpl(Name, RHIPassType::Compute, Desc, &Entry::ComputeDesc);
}

auto PipelineRegistry::Register(StringView Name, const RayTracingPipelineRequest& Desc) -> void {
    RegisterImpl(Name, RHIPassType::RayTracing, Desc, &Entry::RayTracingDesc);
}

auto PipelineRegistry::EnsureRequestStarted(StringView Name) -> void {
    std::scoped_lock Lock(m_Mutex);
    const auto       It = m_Entries.find(String(Name));
    if (It == m_Entries.end())
        return; // Unregistered: no-op; AddPipelinePass / renderer snapshot report the config error.
    auto& E = It->second;
    if (E.Requested)
        return; // Idempotent: start exactly once.
    E.Requested           = true;
    const auto NameString = String(Name);
    switch (E.Kind) {
    case RHIPassType::Graphics:
        StartGraphicsAsyncLocked(E, NameString);
        break;
    case RHIPassType::Compute:
        StartComputeAsyncLocked(E, NameString);
        break;
    case RHIPassType::RayTracing:
        StartRayTracingAsyncLocked(E, NameString);
        break;
    default:
        break;
    }
}

[[nodiscard]] auto PipelineRegistry::IsRegistered(StringView Name) const -> bool {
    std::scoped_lock Lock(m_Mutex);
    return m_Entries.contains(String(Name));
}

[[nodiscard]] auto PipelineRegistry::GetState(StringView Name) const -> RGPipelineState {
    std::scoped_lock Lock(m_Mutex);
    const auto       It = m_Entries.find(String(Name));
    if (It == m_Entries.end())
        return RGPipelineState::Unknown;
    const auto& E = It->second;
    if (!E.Requested)
        return RGPipelineState::Pending; // Registered but not started.
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

[[nodiscard]] auto PipelineRegistry::GetError(StringView Name) const -> std::optional<ErrorMessage> {
    std::scoped_lock Lock(m_Mutex);
    const auto       It = m_Entries.find(String(Name));
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

[[nodiscard]] auto PipelineRegistry::GetReadyGraphics(StringView Name) const -> RHIRef<RHIGraphicsPipeline> {
    return GetReady(Name, RHIPassType::Graphics, &Entry::GraphicsRef);
}

[[nodiscard]] auto PipelineRegistry::GetReadyCompute(StringView Name) const -> RHIRef<RHIComputePipeline> {
    return GetReady(Name, RHIPassType::Compute, &Entry::ComputeRef);
}

[[nodiscard]] auto PipelineRegistry::GetReadyRayTracing(StringView Name) const -> RHIRef<RHIRayTracingPipeline> {
    return GetReady(Name, RHIPassType::RayTracing, &Entry::RayTracingRef);
}

auto PipelineRegistry::StartGraphicsAsyncLocked(Entry& E, String Name) -> void {
    auto PipelineRef   = RHIRef<RHIGraphicsPipeline>::Create();
    auto BindingSetRef = RHIRef<RHIShaderBindingSet>::Create();
    E.GraphicsRef      = PipelineRef; // Entry keeps the ref so GetState / GetReady can observe it.

    // Capture the descriptor by value into the background closure; the entry itself only stores the ref.
    auto Desc          = E.GraphicsDesc;
    auto EnqueueResult = TaskGraph::Get().EnqueueBackground(
        [Desc = std::move(Desc), Name = std::move(Name), PipelineRef, BindingSetRef]() mutable {
            std::vector<Path> IncludeDirs{EnginePaths::Get().EngineShadersDirPath()};

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
                .DepthStencil      = Desc.DepthStencil,
                .ColorAttachments  = Desc.ColorAttachments,
                .DepthFormat       = Desc.DepthFormat,
            };
            auto EnqueueResult = TaskGraph::Get().EnqueueTask(
                ThreadQueue::RHI,
                [PipelineRef, BindingSetRef, Name = std::move(Name), PipelineDesc = std::move(PipelineDesc)]() mutable {
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

auto PipelineRegistry::StartComputeAsyncLocked(Entry& E, String Name) -> void {
    auto PipelineRef   = RHIRef<RHIComputePipeline>::Create();
    auto BindingSetRef = RHIRef<RHIShaderBindingSet>::Create();
    E.ComputeRef       = PipelineRef;

    auto Desc          = E.ComputeDesc;
    auto EnqueueResult = TaskGraph::Get().EnqueueBackground(
        [Desc = std::move(Desc), Name = std::move(Name), PipelineRef, BindingSetRef]() mutable {
            std::vector<Path> IncludeDirs{EnginePaths::Get().EngineShadersDirPath()};

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
                .Program                 = std::move(*Program),
                .SpecializationConstants = Desc.SpecializationConstants,
            };
            auto EnqueueResult = TaskGraph::Get().EnqueueTask(
                ThreadQueue::RHI,
                [PipelineRef, BindingSetRef, Name = std::move(Name), PipelineDesc = std::move(PipelineDesc)]() mutable {
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

auto PipelineRegistry::StartRayTracingAsyncLocked(Entry& E, String Name) -> void {
    auto PipelineRef   = RHIRef<RHIRayTracingPipeline>::Create();
    auto BindingSetRef = RHIRef<RHIShaderBindingSet>::Create();
    E.RayTracingRef    = PipelineRef;

    auto Desc          = E.RayTracingDesc;
    auto EnqueueResult = TaskGraph::Get().EnqueueBackground(
        [Desc = std::move(Desc), Name = std::move(Name), PipelineRef, BindingSetRef]() mutable {
            std::vector<Path> IncludeDirs{EnginePaths::Get().EngineShadersDirPath()};

            auto Program = ShaderCompiler::Get().CompileRayTracing(RayTracingCompileDesc{
                .RayGeneration = Desc.RayGeneration,
                .MissEntries   = Desc.MissEntries,
                .HitGroups     = Desc.HitGroups,
                .IncludeDirs   = IncludeDirs,
            });
            if (!Program) {
                auto Error = Program.error().Append(
                    Format("Ray-tracing pipeline shader '{}'", Desc.RayGeneration.SourcePath.string()));
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
                [PipelineRef, BindingSetRef, Name = std::move(Name), PipelineDesc = std::move(PipelineDesc)]() mutable {
                    PublishPipelineOnRhiThread(
                        PipelineRef,
                        BindingSetRef,
                        std::move(Name),
                        std::move(PipelineDesc),
                        ShaderStage::RayGeneration | ShaderStage::Intersection | ShaderStage::AnyHit |
                            ShaderStage::ClosestHit | ShaderStage::Miss | ShaderStage::Callable,
                        [](const String& PipelineName, const RHIRayTracingPipelineDesc& PipelineDescArg) {
                            return RHIRenderDevice::Get().CreateRayTracingPipeline(PipelineName, PipelineDescArg);
                        });
                });
            if (!EnqueueResult)
                PipelineRef.MarkFailed(EnqueueResult.error());
        });
    if (!EnqueueResult)
        PipelineRef.MarkFailed(EnqueueResult.error());
}

} // namespace SoulEngine
