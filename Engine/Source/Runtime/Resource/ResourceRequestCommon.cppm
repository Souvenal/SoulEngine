export module Resource:RequestCommon;

export import Core;
export import RHI;
import :Context;
export import :Types;
import TaskGraph;
export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Resource {

template <ManagedRHIResource T>
auto PublishResourceReady(ResourceContext& Context,
                          ResourceGeneration Generation,
                          const String& Key,
                          Resource<T> Resource) -> void {
    if (Context.IsShutdownRequested()) {
        LogDebug("Async {} ready discarded after shutdown '{}'", ResourceTraits<T>::Info.Label, Key);
        return;
    }

    if (!Context.PublishReady(Key, Generation, std::move(Resource))) {
        LogDebug("Stale async {} ready discarded '{}'", ResourceTraits<T>::Info.Label, Key);
        return;
    }

    LogInfo("Async {} ready '{}'", ResourceTraits<T>::Info.Label, Key);
}

template <ManagedRHIResource T>
auto PublishResourceFailed(ResourceContext& Context,
                           ResourceGeneration Generation,
                           const String& Key,
                           ErrorMessage Error) -> void {
    if (Context.IsShutdownRequested()) {
        LogDebug("Async {} failure discarded after shutdown '{}'", ResourceTraits<T>::Info.Label, Key);
        return;
    }

    auto Message = Error.ToString();
    if (!Context.PublishFailed<T>(Key, Generation, std::move(Error))) {
        LogDebug("Stale async {} failure discarded '{}': {}", ResourceTraits<T>::Info.Label, Key, Message);
        return;
    }

    LogWarning("Async {} failed '{}': {}", ResourceTraits<T>::Info.Label, Key, Message);
}

template <ManagedRHIResource T>
struct ResourceWorkStart {
    ResourceHandle<T> Handle          = {};
    TaskGraph*        Graph           = nullptr;
    bool              ShouldStartWork = false;
};

template <ManagedRHIResource T>
[[nodiscard]] auto BeginResourceWork(ResourceContext& Context, const String& Key) -> ResourceWorkStart<T> {
    if (Context.IsShutdownRequested()) {
        LogWarning("{} request rejected after shutdown '{}'", ResourceTraits<T>::Info.Label, Key);
        return {};
    }

    auto Request = Context.CreateOrGet<T>(Key);
    auto Handle  = Request.Handle;
    if (!Request.ShouldStartWork) {
        return ResourceWorkStart<T>{
            .Handle          = Handle,
            .ShouldStartWork = false,
        };
    }

    auto* Graph = Context.GetTaskGraph();
    if (!Graph) {
        PublishResourceFailed<T>(
            Context, Handle.GetGeneration(), Key, ErrorMessage(Format("TaskGraph not initialized for '{}'", Key)));
        return ResourceWorkStart<T>{
            .Handle          = Handle,
            .ShouldStartWork = false,
        };
    }

    return ResourceWorkStart<T>{
        .Handle          = Handle,
        .Graph           = Graph,
        .ShouldStartWork = true,
    };
}

template <GpuPendingManagedRHIResource T>
auto PublishResourceGpuPending(ResourceContext& Context,
                               ResourceGeneration Generation,
                               const String& Key,
                               Resource<T> Resource,
                               RHI::GpuCompletionToken UploadCompletion) -> void {
    if (Context.PublishGpuPending<T>(Key, Generation, std::move(Resource), UploadCompletion))
        LogDebug("Async {} GPU pending '{}'", ResourceTraits<T>::Info.Label, Key);
}

template <ManagedRHIResource T>
[[nodiscard]] auto MarkResourceRhiCommitting(ResourceContext& Context,
                                             const String& Key,
                                             ResourceGeneration Generation) -> bool {
    if (Context.MarkRhiCommitting<T>(Key, Generation))
        return true;

    LogDebug("Stale async {} RHI commit discarded '{}'", ResourceTraits<T>::Info.Label, Key);
    return false;
}

} // namespace SoulEngine::Resource
