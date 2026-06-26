export module Resource:PipelineRequests;

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

    Key += Format("|vbind={}:{}", Req.VertexInputLayout.Binding, Req.VertexInputLayout.Stride);
    for (const auto& Attribute : Req.VertexInputLayout.Attributes)
        Key += Format("|attr={}:{}:{}", Attribute.Location, static_cast<Uint8>(Attribute.Format), Attribute.Offset);

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

    auto Vert = SC::ShaderCompiler::Get().GetOrCompile(Req.VertEntry);
    if (!Vert)
        return std::unexpected(Vert.error().Append(Format("Graphics pipeline vertex shader '{}'/'{}'",
                                                          Req.VertEntry.SourcePath.string(),
                                                          Req.VertEntry.EntryPoint)));

    auto Frag = SC::ShaderCompiler::Get().GetOrCompile(Req.FragEntry);
    if (!Frag)
        return std::unexpected(Frag.error().Append(Format("Graphics pipeline fragment shader '{}'/'{}'",
                                                          Req.FragEntry.SourcePath.string(),
                                                          Req.FragEntry.EntryPoint)));

    return PreparedGraphicsPipeline{
        .Desc =
            RHI::GraphicsPipelineDesc{
                .VertexProgram     = std::move(*Vert),
                .FragmentProgram   = std::move(*Frag),
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

} // namespace SoulEngine::Resource
