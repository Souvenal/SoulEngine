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

export [[nodiscard]] auto RequestGraphicsPipeline(StringView Name, const GraphicsPipelineRequest& Req)
    -> std::expected<RHIRef<RHIGraphicsPipeline>, ErrorMessage> {
    auto PipelineRef   = RHIRef<RHIGraphicsPipeline>::Create();
    auto EnqueueResult = TaskGraph::Get().EnqueueBackground([Req, PipelineRef, Name = String(Name)]() mutable {
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

        auto       PipelineDesc = RHIGraphicsPipelineDesc{
            .Program           = std::move(*Program),
            .VertexInputLayout = Req.VertexInputLayout,
            .Topology          = Req.Topology,
            .Rasterizer        = Req.Rasterizer,
            .Blend             = Req.Blend,
            .DepthStencil      = Req.DepthStencil,
            .ColorFormats      = Req.ColorFormats,
            .DepthFormat       = Req.DepthFormat,
        };
        auto EnqueueResult = TaskGraph::Get().EnqueueTask(
            ThreadQueue::RHI,
            [PipelineRef, Name = std::move(Name), PipelineDesc = std::move(PipelineDesc)]() mutable {
                auto Created = RHIRenderDevice::Get().CreateGraphicsPipeline(Name, PipelineDesc);
                if (!Created) {
                    PipelineRef.MarkFailed(Created.error());
                    return;
                }
                if (auto Publish = PipelineRef.Publish(std::move(*Created), RHIRefState::Ready); !Publish)
                    PipelineRef.MarkFailed(Publish.error());
            });
        if (!EnqueueResult)
            PipelineRef.MarkFailed(EnqueueResult.error());
    });
    if (!EnqueueResult) {
        PipelineRef.MarkFailed(EnqueueResult.error());
        return std::unexpected(EnqueueResult.error());
    }
    return PipelineRef;
}

export [[nodiscard]] auto RequestRayTracingPipeline(StringView Name, const RayTracingPipelineRequest& Req)
    -> std::expected<RHIRef<RHIRayTracingPipeline>, ErrorMessage> {
    auto PipelineRef   = RHIRef<RHIRayTracingPipeline>::Create();
    auto EnqueueResult = TaskGraph::Get().EnqueueBackground([Req, PipelineRef, Name = String(Name)]() mutable {
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
        auto PipelineDesc = RHIRayTracingPipelineDesc{
            .Program = std::move(*Program), .MaxRecursionDepth = Req.MaxRecursionDepth};
        auto EnqueueResult = TaskGraph::Get().EnqueueTask(
            ThreadQueue::RHI,
            [PipelineRef, Name = std::move(Name), PipelineDesc = std::move(PipelineDesc)]() mutable {
                auto Created = RHIRenderDevice::Get().CreateRayTracingPipeline(Name, PipelineDesc);
                if (!Created) {
                    PipelineRef.MarkFailed(Created.error());
                    return;
                }
                if (auto Publish = PipelineRef.Publish(std::move(*Created), RHIRefState::Ready); !Publish)
                    PipelineRef.MarkFailed(Publish.error());
            });
        if (!EnqueueResult)
            PipelineRef.MarkFailed(EnqueueResult.error());
    });
    if (!EnqueueResult) {
        PipelineRef.MarkFailed(EnqueueResult.error());
        return std::unexpected(EnqueueResult.error());
    }
    return PipelineRef;
}

} // namespace SoulEngine
