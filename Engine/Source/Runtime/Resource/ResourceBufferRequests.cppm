export module Resource:BufferRequests;

export import Core;
export import RHI;
import :Context;
import :RequestCommon;
export import :Types;
import TaskGraph;
export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Resource {

[[nodiscard]] auto SubmitVertexBufferRequest(ResourceContext& Context, String Key, const RHI::VertexBufferDesc& Desc)
    -> ResourceHandle<RHI::VertexBuffer> {
    auto Work   = BeginResourceWork<RHI::VertexBuffer>(Context, Key);
    auto Handle = Work.Handle;
    if (!Work.ShouldStartWork)
        return Handle;

    LogDebug("Vertex buffer requested '{}'", Key);

    // Copy caller's data for async RHI task
    Uint64             Size = Desc.VertexCount * Desc.Stride;
    std::vector<Uint8> DataCopy(static_cast<const Uint8*>(Desc.Data), static_cast<const Uint8*>(Desc.Data) + Size);

    auto* Graph      = Work.Graph;
    auto* ContextPtr = &Context;
    Graph->Enqueue(ThreadQueue::RHI,
                   [ContextPtr, Generation = Handle.GetGeneration(), Key = String(Key), Desc, DataCopy = std::move(DataCopy)] {
                       auto& Context = *ContextPtr;
                       if (Context.IsShutdownRequested()) {
                           LogDebug("Async vertex buffer RHI commit discarded after shutdown '{}'", Key);
                           return;
                       }

                       if (!MarkResourceRhiCommitting<RHI::VertexBuffer>(Context, Key, Generation))
                           return;

                       RHI::VertexBufferDesc BufDesc = Desc;
                       BufDesc.Data                  = DataCopy.data();

                       auto Result = RHI::RenderDevice::Get().CreateVertexBuffer(BufDesc);
                       if (!Result) {
                           PublishResourceFailed<RHI::VertexBuffer>(
                               Context,
                               Generation,
                               Key,
                               Result.error().Append(Format("Failed to create vertex buffer '{}'", Key)));
                           return;
                       }

                       PublishResourceGpuPending<RHI::VertexBuffer>(
                           Context,
                           Generation,
                           Key,
                           Resource<RHI::VertexBuffer>{.Object = std::move(Result->Buffer)},
                           Result->UploadCompletion);
                   });

    return Handle;
}

[[nodiscard]] auto SubmitIndexBufferRequest(ResourceContext& Context, String Key, const RHI::IndexBufferDesc& Desc)
    -> ResourceHandle<RHI::IndexBuffer> {
    auto Work   = BeginResourceWork<RHI::IndexBuffer>(Context, Key);
    auto Handle = Work.Handle;
    if (!Work.ShouldStartWork)
        return Handle;

    LogDebug("Index buffer requested '{}'", Key);

    // Copy caller's data for async RHI task
    Uint64             Size = Desc.IndexCount * 4ULL;
    std::vector<Uint8> DataCopy(static_cast<const Uint8*>(Desc.Data), static_cast<const Uint8*>(Desc.Data) + Size);

    auto* Graph      = Work.Graph;
    auto* ContextPtr = &Context;
    Graph->Enqueue(ThreadQueue::RHI,
                   [ContextPtr, Generation = Handle.GetGeneration(), Key = String(Key), Desc, DataCopy = std::move(DataCopy)] {
                       auto& Context = *ContextPtr;
                       if (Context.IsShutdownRequested()) {
                           LogDebug("Async index buffer RHI commit discarded after shutdown '{}'", Key);
                           return;
                       }

                       if (!MarkResourceRhiCommitting<RHI::IndexBuffer>(Context, Key, Generation))
                           return;

                       RHI::IndexBufferDesc BufDesc = Desc;
                       BufDesc.Data                 = DataCopy.data();

                       auto Result = RHI::RenderDevice::Get().CreateIndexBuffer(BufDesc);
                       if (!Result) {
                           PublishResourceFailed<RHI::IndexBuffer>(
                               Context,
                               Generation,
                               Key,
                               Result.error().Append(Format("Failed to create index buffer '{}'", Key)));
                           return;
                       }

                       PublishResourceGpuPending<RHI::IndexBuffer>(
                           Context,
                           Generation,
                           Key,
                           Resource<RHI::IndexBuffer>{.Object = std::move(Result->Buffer)},
                           Result->UploadCompletion);
                   });

    return Handle;
}

} // namespace SoulEngine::Resource
