export module Resource:Buffer;

export import Core;
export import RHI;
import :Context;
import :RequestCommon;
export import :Types;
import TaskGraph;
export import std;

namespace SoulEngine {
namespace {

[[nodiscard]] auto ValidateVertexBufferDesc(const RHIVertexBufferDesc& Desc) -> std::expected<void, ErrorMessage> {
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

[[nodiscard]] auto ValidateIndexBufferDesc(const RHIIndexBufferDesc& Desc) -> std::expected<void, ErrorMessage> {
    if (!Desc.Data)
        return std::unexpected(ErrorMessage("Index buffer data pointer is null"));
    if (Desc.IndexCount == 0)
        return std::unexpected(ErrorMessage("Index buffer count is zero"));
    if (Desc.IndexCount > std::numeric_limits<Uint32>::max())
        return std::unexpected(ErrorMessage("Index buffer count exceeds draw limit"));

    return {};
}

} // namespace


[[nodiscard]] auto SubmitVertexBufferRequest(ResourceContext& Context, String Key, const RHIVertexBufferDesc& Desc)
    -> ResourceHandle<RHIVertexBuffer> {
    auto Work   = BeginResourceWork<RHIVertexBuffer>(Context, Key);
    auto Handle = Work.Handle;
    if (!Work.ShouldStartWork)
        return Handle;

    if (auto R = ValidateVertexBufferDesc(Desc); !R) {
        PublishResourceFailed<RHIVertexBuffer>(
            Context, Handle.GetGeneration(), Key, R.error().Append(Format("Invalid vertex buffer request '{}'", Key)));
        return Handle;
    }

    LogDebug("Vertex buffer requested '{}'", Key);

    // Copy caller's data for async RHI task
    Uint64             Size = Desc.VertexCount * Desc.Stride;
    std::vector<Uint8> DataCopy(static_cast<const Uint8*>(Desc.Data), static_cast<const Uint8*>(Desc.Data) + Size);

    auto* ContextPtr = &Context;
    auto EnqueueResult = TaskGraph::Get().Enqueue(
        ThreadQueue::RHI,
        [ContextPtr, Generation = Handle.GetGeneration(), Key = String(Key), Desc, DataCopy = std::move(DataCopy)] {
                       auto& Context = *ContextPtr;
                       if (Context.IsShutdownRequested()) {
                           LogDebug("Async vertex buffer RHI commit discarded after shutdown '{}'", Key);
                           return;
                       }

                       if (!MarkResourceRhiCommitting<RHIVertexBuffer>(Context, Key, Generation))
                           return;

                       RHIVertexBufferDesc BufDesc = Desc;
                       BufDesc.Data                  = DataCopy.data();

                       auto Result = RHIRenderDevice::Get().CreateVertexBuffer(BufDesc);
                       if (!Result) {
                           PublishResourceFailed<RHIVertexBuffer>(
                               Context,
                               Generation,
                               Key,
                               Result.error().Append(Format("Failed to create vertex buffer '{}'", Key)));
                           return;
                       }

                       PublishResourceGpuPending<RHIVertexBuffer>(
                           Context,
                           Generation,
                           Key,
                           Resource<RHIVertexBuffer>{.Object = std::move(Result->Buffer)},
                           Result->UploadCompletion);
        });
    if (!EnqueueResult) {
        PublishResourceFailed<RHIVertexBuffer>(
            Context,
            Handle.GetGeneration(),
            Key,
            EnqueueResult.error().Append(
                Format("Failed to enqueue async {} work '{}'", ResourceTraits<RHIVertexBuffer>::Info.Label, Key)));
    }

    return Handle;
}

[[nodiscard]] auto SubmitIndexBufferRequest(ResourceContext& Context, String Key, const RHIIndexBufferDesc& Desc)
    -> ResourceHandle<RHIIndexBuffer> {
    auto Work   = BeginResourceWork<RHIIndexBuffer>(Context, Key);
    auto Handle = Work.Handle;
    if (!Work.ShouldStartWork)
        return Handle;

    if (auto R = ValidateIndexBufferDesc(Desc); !R) {
        PublishResourceFailed<RHIIndexBuffer>(
            Context, Handle.GetGeneration(), Key, R.error().Append(Format("Invalid index buffer request '{}'", Key)));
        return Handle;
    }

    LogDebug("Index buffer requested '{}'", Key);

    // Copy caller's data for async RHI task
    Uint64             Size = Desc.IndexCount * 4ULL;
    std::vector<Uint8> DataCopy(static_cast<const Uint8*>(Desc.Data), static_cast<const Uint8*>(Desc.Data) + Size);

    auto* ContextPtr = &Context;
    auto EnqueueResult = TaskGraph::Get().Enqueue(
        ThreadQueue::RHI,
        [ContextPtr, Generation = Handle.GetGeneration(), Key = String(Key), Desc, DataCopy = std::move(DataCopy)] {
                       auto& Context = *ContextPtr;
                       if (Context.IsShutdownRequested()) {
                           LogDebug("Async index buffer RHI commit discarded after shutdown '{}'", Key);
                           return;
                       }

                       if (!MarkResourceRhiCommitting<RHIIndexBuffer>(Context, Key, Generation))
                           return;

                       RHIIndexBufferDesc BufDesc = Desc;
                       BufDesc.Data                 = DataCopy.data();

                       auto Result = RHIRenderDevice::Get().CreateIndexBuffer(BufDesc);
                       if (!Result) {
                           PublishResourceFailed<RHIIndexBuffer>(
                               Context,
                               Generation,
                               Key,
                               Result.error().Append(Format("Failed to create index buffer '{}'", Key)));
                           return;
                       }

                       PublishResourceGpuPending<RHIIndexBuffer>(
                           Context,
                           Generation,
                           Key,
                           Resource<RHIIndexBuffer>{.Object = std::move(Result->Buffer)},
                           Result->UploadCompletion);
        });
    if (!EnqueueResult) {
        PublishResourceFailed<RHIIndexBuffer>(
            Context,
            Handle.GetGeneration(),
            Key,
            EnqueueResult.error().Append(
                Format("Failed to enqueue async {} work '{}'", ResourceTraits<RHIIndexBuffer>::Info.Label, Key)));
    }

    return Handle;
}

} // namespace SoulEngine
