export module Resource:RenderTarget;

export import Core;
export import RHI;
import :Context;
import :RequestCommon;
export import :Types;
import TaskGraph;
export import std;

export namespace SoulEngine {

[[nodiscard]] auto SubmitRenderTargetRequest(ResourceContext& Context, String Key, const RHIRenderTargetDesc& Desc)
    -> ResourceHandle<RHIRenderTarget> {
    auto Work   = BeginResourceWork<RHIRenderTarget>(Context, Key);
    auto Handle = Work.Handle;
    if (!Work.ShouldStartWork)
        return Handle;

    LogDebug("Render target requested '{}' ({}x{})", Key, Desc.Width, Desc.Height);

    // No background decode — render targets are created empty.
    // Enqueue RHI creation directly.
    auto* Graph      = Work.Graph;
    auto* ContextPtr = &Context;
    Graph->Enqueue(ThreadQueue::RHI, [ContextPtr, Generation = Handle.GetGeneration(), Key = String(Key), Desc] {
        auto& Context = *ContextPtr;
        if (Context.IsShutdownRequested()) {
            LogDebug("Async render target RHI commit discarded after shutdown '{}'", Key);
            return;
        }

        if (!MarkResourceRhiCommitting<RHIRenderTarget>(Context, Key, Generation))
            return;

        auto Result = RHIRenderDevice::Get().CreateRenderTarget(Desc);
        if (!Result) {
            PublishResourceFailed<RHIRenderTarget>(
                Context, Generation, Key, Result.error().Append(Format("Failed to create render target '{}'", Key)));
            return;
        }

        PublishResourceReady<RHIRenderTarget>(
            Context, Generation, Key, Resource<RHIRenderTarget>{.Object = std::move(Result->Texture)});
    });

    return Handle;
}

} // namespace SoulEngine
