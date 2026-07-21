export module Resource:Pipeline;

export import Core;
export import RHI;
import :Context;
import :RequestCommon;
export import :Types;
import ShaderCompiler;
import TaskGraph;
export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Resource {

[[nodiscard]] auto MakePipelineKey(const GraphicsPipelineRequest& Req) -> String {
    String Key = Format("vp={}:{}:{}|fp={}:{}:{}|topo={}|cf={}|df={}|rs={}:{}:{}|ds={}:{}",
                        Req.VertEntry.SourcePath.lexically_normal().string(),
                        Req.VertEntry.EntryPoint,
                        static_cast<Uint8>(Req.VertEntry.Backend),
                        Req.FragEntry.SourcePath.lexically_normal().string(),
                        Req.FragEntry.EntryPoint,
                        static_cast<Uint8>(Req.FragEntry.Backend),
                        static_cast<Uint8>(Req.Topology),
                        static_cast<Uint8>(Req.ColorFormat),
                        static_cast<Uint8>(Req.DepthFormat),
                        Req.Rasterizer.FillMode,
                        Req.Rasterizer.CullMode,
                        Req.Rasterizer.LineWidth,
                        Req.DepthStencil.DepthTestEnable,
                        Req.DepthStencil.DepthWriteEnable);

    for (const auto& Binding : Req.VertexInputLayout.Bindings)
        Key += Format("|vbind={}:{}", Binding.Binding, Binding.Stride);
    for (const auto& Attribute : Req.VertexInputLayout.Attributes)
        Key += Format(
            "|attr={}:{}:{}:{}", Attribute.Location, Attribute.Binding, static_cast<Uint8>(Attribute.Format), Attribute.Offset);

    for (const auto& Attachment : Req.Blend.Attachments)
        Key += Format("|blend={}", Attachment.BlendEnable);

    return Key;
}

struct PreparedGraphicsPipeline {
    RHI::GraphicsPipelineDesc Desc = {};
};

[[nodiscard]] auto PrepareGraphicsPipeline(const GraphicsPipelineRequest& Req)
    -> std::expected<PreparedGraphicsPipeline, ErrorMessage> {
    namespace SC = SoulEngine::ShaderCompiler;

    const auto&       Cfg = ConfigManager::Get();
    std::vector<Path> IncludeDirs{
        Cfg.EngineShadersDirPath(),
        Cfg.CurrentApplicationDir() / "Shaders",
    };

    auto Program = SC::ShaderCompiler::Get().CompileGraphics(SC::GraphicsCompileDesc{
        .Vertex      = Req.VertEntry,
        .Fragment    = Req.FragEntry,
        .IncludeDirs = IncludeDirs,
    });
    if (!Program) {
        return std::unexpected(Program.error().Append(Format("Graphics pipeline shaders '{}'/'{}' + '{}'/'{}'",
                                                             Req.VertEntry.SourcePath.string(),
                                                             Req.VertEntry.EntryPoint,
                                                             Req.FragEntry.SourcePath.string(),
                                                             Req.FragEntry.EntryPoint)));
    }

    return PreparedGraphicsPipeline{
        .Desc =
            RHI::GraphicsPipelineDesc{
                .Program           = std::move(*Program),
                .VertexInputLayout = Req.VertexInputLayout,
                .Topology          = Req.Topology,
                .Rasterizer        = Req.Rasterizer,
                .Blend             = Req.Blend,
                .DepthStencil      = Req.DepthStencil,
                .ColorFormat       = Req.ColorFormat,
                .DepthFormat       = Req.DepthFormat,
            },
    };
}

[[nodiscard]] auto SubmitGraphicsPipelineRequest(ResourceContext& Context, const GraphicsPipelineRequest& Req)
    -> ResourceHandle<RHI::GraphicsPipeline> {
    const auto Key = MakePipelineKey(Req);

    auto Work   = BeginResourceWork<RHI::GraphicsPipeline>(Context, Key);
    auto Handle = Work.Handle;
    if (!Work.ShouldStartWork)
        return Handle;

    LogDebug("Graphics pipeline requested '{}'", Key);

    auto* Graph      = Work.Graph;
    auto* ContextPtr = &Context;
    Graph->EnqueueBackground([ContextPtr, Graph, Generation = Handle.GetGeneration(), Key, Req] {
        auto& Context = *ContextPtr;
        if (Context.IsShutdownRequested()) {
            LogDebug("Async graphics pipeline compile discarded after shutdown '{}'", Key);
            return;
        }

        auto Prepared = PrepareGraphicsPipeline(Req);
        if (!Prepared) {
            PublishResourceFailed<RHI::GraphicsPipeline>(Context, Generation, Key, Prepared.error());
            return;
        }

        if (Context.IsShutdownRequested()) {
            LogDebug("Async graphics pipeline creation discarded after shutdown '{}'", Key);
            return;
        }

        Graph->Enqueue(ThreadQueue::RHI, [ContextPtr, Generation, Key, Prepared = std::move(*Prepared)] {
            auto& Context = *ContextPtr;
            if (Context.IsShutdownRequested()) {
                LogDebug("Async graphics pipeline publish discarded after shutdown '{}'", Key);
                return;
            }

            if (!MarkResourceRhiCommitting<RHI::GraphicsPipeline>(Context, Key, Generation))
                return;

            auto PipeResult = RHI::RenderDevice::Get().CreateGraphicsPipeline(Prepared.Desc);
            if (!PipeResult) {
                PublishResourceFailed<RHI::GraphicsPipeline>(
                    Context,
                    Generation,
                    Key,
                    PipeResult.error().Append(Format("Failed to create graphics pipeline '{}'", Key)));
                return;
            }

            PublishResourceReady<RHI::GraphicsPipeline>(
                Context, Generation, Key, Resource<RHI::GraphicsPipeline>{.Object = std::move(*PipeResult)});
        });
    });

    return Handle;
}

[[nodiscard]] auto MakeRayTracingPipelineKey(const RayTracingPipelineRequest& Req) -> String {
    String Key = Format("rt/raygen={}:{}:{}|depth={}", Req.RayGeneration.SourcePath.lexically_normal().string(),
                        Req.RayGeneration.EntryPoint, static_cast<Uint8>(Req.RayGeneration.Backend), Req.MaxRecursionDepth);
    for (const auto& Miss : Req.MissEntries)
        Key += Format("|miss={}:{}:{}", Miss.SourcePath.lexically_normal().string(), Miss.EntryPoint, static_cast<Uint8>(Miss.Backend));
    for (const auto& Group : Req.HitGroups) {
        Key += Format("|hit={}", static_cast<Uint8>(Group.Type));
        if (Group.ClosestHit)
            Key += Format(":{}", Group.ClosestHit->EntryPoint);
    }
    return Key;
}

[[nodiscard]] auto SubmitRayTracingPipelineRequest(ResourceContext& Context, const RayTracingPipelineRequest& Req)
    -> ResourceHandle<RHI::RayTracingPipeline> {
    const auto Key = MakeRayTracingPipelineKey(Req);
    auto Work = BeginResourceWork<RHI::RayTracingPipeline>(Context, Key);
    if (!Work.ShouldStartWork)
        return Work.Handle;

    auto* Graph = Work.Graph;
    auto* ContextPtr = &Context;
    Graph->EnqueueBackground([ContextPtr, Graph, Generation = Work.Handle.GetGeneration(), Key, Req] {
        auto& Context = *ContextPtr;
        if (Context.IsShutdownRequested())
            return;
        const auto& Cfg = ConfigManager::Get();
        std::vector<Path> IncludeDirs{Cfg.EngineShadersDirPath(), Cfg.CurrentApplicationDir() / "Shaders"};
        auto Program = ShaderCompiler::ShaderCompiler::Get().CompileRayTracing(ShaderCompiler::RayTracingCompileDesc{
            .RayGeneration = Req.RayGeneration,
            .MissEntries = Req.MissEntries,
            .HitGroups = Req.HitGroups,
            .IncludeDirs = IncludeDirs,
        });
        if (!Program) {
            PublishResourceFailed<RHI::RayTracingPipeline>(Context, Generation, Key, Program.error());
            return;
        }
        Graph->Enqueue(ThreadQueue::RHI, [ContextPtr, Generation, Key, Program = std::move(*Program), MaxDepth = Req.MaxRecursionDepth] mutable {
            auto& Context = *ContextPtr;
            if (Context.IsShutdownRequested() || !MarkResourceRhiCommitting<RHI::RayTracingPipeline>(Context, Key, Generation))
                return;
            auto Pipeline = RHI::RenderDevice::Get().CreateRayTracingPipeline(
                RHI::RayTracingPipelineDesc{.Program = std::move(Program), .MaxRecursionDepth = MaxDepth});
            if (!Pipeline) {
                PublishResourceFailed<RHI::RayTracingPipeline>(Context, Generation, Key, Pipeline.error());
                return;
            }
            PublishResourceReady<RHI::RayTracingPipeline>(Context, Generation, Key, {.Object = std::move(*Pipeline)});
        });
    });
    return Work.Handle;
}

} // namespace SoulEngine::Resource
