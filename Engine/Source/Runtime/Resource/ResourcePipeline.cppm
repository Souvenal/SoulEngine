export module Resource:Pipeline;

export import Core;
export import RHI;
import :Context;
import :RequestCommon;
export import :Types;
import ShaderCompiler;
import TaskGraph;
export import std;

namespace SoulEngine {

[[nodiscard]] auto MakePipelineKey(const GraphicsPipelineRequest& Req) -> String {
    String Key = Format("vp={}:{}:{}|fp={}:{}:{}|topo={}|df={}|rs={}:{}:{}|ds={}:{}",
                        Req.VertEntry.SourcePath.lexically_normal().string(),
                        Req.VertEntry.EntryPoint,
                        static_cast<Uint8>(Req.VertEntry.Backend),
                        Req.FragEntry.SourcePath.lexically_normal().string(),
                        Req.FragEntry.EntryPoint,
                        static_cast<Uint8>(Req.FragEntry.Backend),
                        static_cast<Uint8>(Req.Topology),
                        static_cast<Uint8>(Req.DepthFormat),
                        Req.Rasterizer.FillMode,
                        Req.Rasterizer.CullMode,
                        Req.Rasterizer.LineWidth,
                        Req.DepthStencil.DepthTestEnable,
                        Req.DepthStencil.DepthWriteEnable);

    for (const auto ColorFormat : Req.ColorFormats)
        Key += Format("|mrt={}", static_cast<Uint8>(ColorFormat));
    for (const auto& Binding : Req.VertexInputLayout.Bindings)
        Key += Format("|vbind={}:{}", Binding.Binding, Binding.Stride);
    for (const auto& Attribute : Req.VertexInputLayout.Attributes)
        Key += Format("|attr={}:{}:{}:{}",
                      Attribute.Location,
                      Attribute.Binding,
                      static_cast<Uint8>(Attribute.Format),
                      Attribute.Offset);

    for (const auto& Attachment : Req.Blend.Attachments)
        Key += Format("|blend={}", Attachment.BlendEnable);

    return Key;
}

export [[nodiscard]] auto RequestGraphicsPipeline(const GraphicsPipelineRequest& Req)
    -> std::expected<RHIRef<RHIGraphicsPipeline>, ErrorMessage> {
    auto PipelineRef   = RHIRef<RHIGraphicsPipeline>::Create();
    auto EnqueueResult = TaskGraph::Get().EnqueueBackground([Req, PipelineRef] {
        const auto&       Cfg = ConfigManager::Get();
        std::vector<Path> IncludeDirs{Cfg.EngineShadersDirPath()};

        auto Program = ShaderCompiler::Get().CompileGraphics(GraphicsCompileDesc{
            .Vertex      = Req.VertEntry,
            .Fragment    = Req.FragEntry,
            .IncludeDirs = IncludeDirs,
        });
        if (!Program) {
            auto Error = Program.error().Append(Format("Graphics pipeline shaders '{}'/'{}' + '{}'/'{}'",
                                                       Req.VertEntry.SourcePath.string(),
                                                       Req.VertEntry.EntryPoint,
                                                       Req.FragEntry.SourcePath.string(),
                                                       Req.FragEntry.EntryPoint));
            LogError("Failed to prepare graphics pipeline: {}", Error.ToString());
            PipelineRef.MarkFailed(std::move(Error));
            return;
        }

        auto Created = RHIRenderDevice::Get().CreateGraphicsPipeline(
            RHIGraphicsPipelineDesc{
                .Program           = std::move(*Program),
                .VertexInputLayout = Req.VertexInputLayout,
                .Topology          = Req.Topology,
                .Rasterizer        = Req.Rasterizer,
                .Blend             = Req.Blend,
                .DepthStencil      = Req.DepthStencil,
                .ColorFormats      = Req.ColorFormats,
                .DepthFormat       = Req.DepthFormat,
            },
            PipelineRef);
        if (!Created) {
            LogError("Failed to queue graphics pipeline creation: {}", Created.error().ToString());
            PipelineRef.MarkFailed(Created.error());
            return;
        }
    });
    if (!EnqueueResult) {
        PipelineRef.MarkFailed(EnqueueResult.error());
        return std::unexpected(EnqueueResult.error());
    }
    return PipelineRef;
}

export [[nodiscard]] auto RequestRayTracingPipeline(const RayTracingPipelineRequest& Req)
    -> std::expected<RHIRef<RHIRayTracingPipeline>, ErrorMessage> {
    auto PipelineRef   = RHIRef<RHIRayTracingPipeline>::Create();
    auto EnqueueResult = TaskGraph::Get().EnqueueBackground([Req, PipelineRef] {
        const auto&       Cfg = ConfigManager::Get();
        std::vector<Path> IncludeDirs{Cfg.EngineShadersDirPath()};
        auto              Program = ShaderCompiler::Get().CompileRayTracing(RayTracingCompileDesc{
            .RayGeneration = Req.RayGeneration,
            .MissEntries   = Req.MissEntries,
            .HitGroups     = Req.HitGroups,
            .IncludeDirs   = IncludeDirs,
        });
        if (!Program) {
            LogError("Failed to prepare ray-tracing pipeline: {}", Program.error().ToString());
            PipelineRef.MarkFailed(Program.error());
            return;
        }
        auto Created = RHIRenderDevice::Get().CreateRayTracingPipeline(
            RHIRayTracingPipelineDesc{.Program = std::move(*Program), .MaxRecursionDepth = Req.MaxRecursionDepth},
            PipelineRef);
        if (!Created) {
            LogError("Failed to queue ray-tracing pipeline creation: {}", Created.error().ToString());
            PipelineRef.MarkFailed(Created.error());
            return;
        }
    });
    if (!EnqueueResult) {
        PipelineRef.MarkFailed(EnqueueResult.error());
        return std::unexpected(EnqueueResult.error());
    }
    return PipelineRef;
}

} // namespace SoulEngine
