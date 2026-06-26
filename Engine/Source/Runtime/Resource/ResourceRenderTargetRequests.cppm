export module Resource:RenderTargetRequests;

export import Core;
export import RHI;
import :Context;
import :RequestCommon;
export import :Types;
import TaskGraph;
export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Resource {

[[nodiscard]] auto SubmitRenderTargetRequest(ResourceContext& Context, String Key, const RHI::RenderTargetDesc& Desc)
    -> ResourceHandle<RHI::RenderTarget> {
    auto Work   = BeginResourceWork<RHI::RenderTarget>(Context, Key);
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

        if (!MarkResourceRhiCommitting<RHI::RenderTarget>(Context, Key, Generation))
            return;

        auto Result = RHI::RenderDevice::Get().CreateRenderTarget(Desc);
        if (!Result) {
            PublishResourceFailed<RHI::RenderTarget>(
                Context, Generation, Key, Result.error().Append(Format("Failed to create render target '{}'", Key)));
            return;
        }

        PublishResourceReady<RHI::RenderTarget>(
            Context, Generation, Key, Resource<RHI::RenderTarget>{.Object = std::move(Result->Texture)});
    });

    return Handle;
}

} // namespace SoulEngine::Resource
