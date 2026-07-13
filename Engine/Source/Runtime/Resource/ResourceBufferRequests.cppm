export module Resource:BufferRequests;

export import Core;
export import RHI;
import :Context;
import :RequestCommon;
export import :Types;
import TaskGraph;
export import std;

using namespace SoulEngine::Core;

namespace SoulEngine::Resource {

[[nodiscard]] auto ValidateVertexBufferDesc(const RHI::VertexBufferDesc& Desc) -> std::expected<void, ErrorMessage> {
    if (!Desc.Data)
        return std::unexpected(ErrorMessage("Vertex buffer data pointer is null"));
    if (Desc.VertexCount == 0)
        return std::unexpected(ErrorMessage("Vertex buffer count is zero"));
    if (Desc.VertexCount > std::numeric_limits<Uint32>::max())
        return std::unexpected(ErrorMessage("Vertex buffer count exceeds draw limit"));
    if (Desc.Stride == 0)
        return std::unexpected(ErrorMessage("Vertex buffer stride is zero"));

    return {};
}

[[nodiscard]] auto ValidateIndexBufferDesc(const RHI::IndexBufferDesc& Desc) -> std::expected<void, ErrorMessage> {
    if (!Desc.Data)
        return std::unexpected(ErrorMessage("Index buffer data pointer is null"));
    if (Desc.IndexCount == 0)
        return std::unexpected(ErrorMessage("Index buffer count is zero"));
    if (Desc.IndexCount > std::numeric_limits<Uint32>::max())
        return std::unexpected(ErrorMessage("Index buffer count exceeds draw limit"));

    return {};
}

} // namespace SoulEngine::Resource

export namespace SoulEngine::Resource {

[[nodiscard]] auto SubmitVertexBufferRequest(ResourceContext& Context, String Key, const RHI::VertexBufferDesc& Desc)
    -> ResourceHandle<RHI::VertexBuffer> {
    auto Work   = BeginResourceWork<RHI::VertexBuffer>(Context, Key);
    auto Handle = Work.Handle;
    if (!Work.ShouldStartWork)
        return Handle;

    if (auto R = ValidateVertexBufferDesc(Desc); !R) {
        PublishResourceFailed<RHI::VertexBuffer>(
            Context, Handle.GetGeneration(), Key, R.error().Append(Format("Invalid vertex buffer request '{}'", Key)));
        return Handle;
    }

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

    if (auto R = ValidateIndexBufferDesc(Desc); !R) {
        PublishResourceFailed<RHI::IndexBuffer>(
            Context, Handle.GetGeneration(), Key, R.error().Append(Format("Invalid index buffer request '{}'", Key)));
        return Handle;
    }

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
