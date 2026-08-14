export module Resource:Pipeline;

export import Core;
export import RHI;
import :Context;
import :RequestCommon;
export import :Types;
import ShaderCompiler;
import TaskGraph;
export import std;

export namespace SoulEngine {

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

struct PreparedGraphicsPipeline {
    RHIGraphicsPipelineDesc Desc = {};
};

[[nodiscard]] auto PrepareGraphicsPipeline(const GraphicsPipelineRequest& Req)
    -> std::expected<PreparedGraphicsPipeline, ErrorMessage> {
    const auto&       Cfg = ConfigManager::Get();
    std::vector<Path> IncludeDirs{Cfg.EngineShadersDirPath()};

    auto Program = ShaderCompiler::Get().CompileGraphics(GraphicsCompileDesc{
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
            RHIGraphicsPipelineDesc{
                .Program           = std::move(*Program),
                .VertexInputLayout = Req.VertexInputLayout,
                .Topology          = Req.Topology,
                .Rasterizer        = Req.Rasterizer,
                .Blend             = Req.Blend,
                .DepthStencil      = Req.DepthStencil,
                .ColorFormat       = Req.ColorFormat,
                .ColorFormats      = Req.ColorFormats,
                .DepthFormat       = Req.DepthFormat,
            },
    };
}

[[nodiscard]] auto SubmitGraphicsPipelinePreparation(
    const GraphicsPipelineRequest& Req,
    std::function<void(RHIRef<RHIGraphicsPipeline>)> OnCreated)
    -> std::expected<void, ErrorMessage> {
    auto EnqueueResult = TaskGraph::Get().EnqueueBackground([Req, OnCreated = std::move(OnCreated)] mutable {
        auto Prepared = PrepareGraphicsPipeline(Req);
        if (!Prepared) {
            LogError("Failed to prepare graphics pipeline: {}", Prepared.error().ToString());
            return;
        }

        auto Created = RHIRenderDevice::Get().CreateGraphicsPipeline(Prepared->Desc);
        if (!Created) {
            LogError("Failed to queue graphics pipeline creation: {}", Created.error().ToString());
            return;
        }

        if (auto Delivery = TaskGraph::Get().Enqueue(
                ThreadQueue::Render,
                [OnCreated = std::move(OnCreated), Pipeline = std::move(*Created)] mutable {
                    OnCreated(std::move(Pipeline));
                });
            !Delivery) {
            LogError("Failed to deliver graphics pipeline creation result: {}", Delivery.error().ToString());
        }
    });
    if (!EnqueueResult)
        return std::unexpected(EnqueueResult.error());
    return {};
}

[[nodiscard]] auto SubmitRayTracingPipelinePreparation(
    const RayTracingPipelineRequest& Req,
    std::function<void(RHIRef<RHIRayTracingPipeline>)> OnCreated)
    -> std::expected<void, ErrorMessage> {
    auto EnqueueResult = TaskGraph::Get().EnqueueBackground([Req, OnCreated = std::move(OnCreated)] mutable {
        const auto& Cfg = ConfigManager::Get();
        std::vector<Path> IncludeDirs{Cfg.EngineShadersDirPath()};
        auto Program = ShaderCompiler::Get().CompileRayTracing(RayTracingCompileDesc{
            .RayGeneration = Req.RayGeneration,
            .MissEntries = Req.MissEntries,
            .HitGroups = Req.HitGroups,
            .IncludeDirs = IncludeDirs,
        });
        if (!Program) {
            LogError("Failed to prepare ray-tracing pipeline: {}", Program.error().ToString());
            return;
        }
        auto Created = RHIRenderDevice::Get().CreateRayTracingPipeline(
            RHIRayTracingPipelineDesc{.Program = std::move(*Program), .MaxRecursionDepth = Req.MaxRecursionDepth});
        if (!Created) {
            LogError("Failed to queue ray-tracing pipeline creation: {}", Created.error().ToString());
            return;
        }
        if (auto Delivery = TaskGraph::Get().Enqueue(
                ThreadQueue::Render,
                [OnCreated = std::move(OnCreated), Pipeline = std::move(*Created)] mutable {
                    OnCreated(std::move(Pipeline));
                });
            !Delivery) {
            LogError("Failed to deliver ray-tracing pipeline creation result: {}", Delivery.error().ToString());
        }
    });
    if (!EnqueueResult)
        return std::unexpected(EnqueueResult.error());
    return {};
}

} // namespace SoulEngine
