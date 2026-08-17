module;

#include <hlsl++.h>

export module Resource:AccelerationStructure;

export import Core;
export import RHI;
import :Context;
import :Geometry;  // NEW: Import GeometryManager
import :RequestCommon;
export import :Types;
import TaskGraph;
export import std;

namespace SoulEngine {
namespace {

[[nodiscard]] auto BuildBottomLevelAccelerationStructureKey(const ResourceHandle<ResourceMesh>&            MeshHandle,
                                                            const BottomLevelAccelerationStructureRequest& Request)
    -> String {
    return Format("rt-blas/mesh={}/generation={}/policy={}",
                  MeshHandle.GetKey(),
                  MeshHandle.GetGeneration(),
                  static_cast<Uint32>(Request.GeometryPolicy));
}

[[nodiscard]] auto BuildTopLevelAccelerationStructureKey(StringView ScopeKey) -> String {
    return Format("rt-tlas/scope={}", ScopeKey);
}

struct PendingBottomLevelAccelerationStructureRequest {
    ResourceContext*                        Context    = nullptr;
    ResourceGeneration                      Generation = 0;
    String                                  Key        = {};
    BottomLevelAccelerationStructureRequest Request    = {};
    ResourceRef<ResourceMesh>               MeshRef    = {};
};

[[nodiscard]] auto IsDependencyFailed(ResourceContext& Context, const ResourceHandle<ResourceMesh>& Handle) -> bool {
    const auto State = Context.GetState(Handle);
    return State == ResourceState::Failed || State == ResourceState::Stale || State == ResourceState::Unknown;
}

[[nodiscard]] auto
WaitForBottomLevelAccelerationStructureDependencies(const SPtr<PendingBottomLevelAccelerationStructureRequest>& Pending)
    -> bool {
    auto& Context = *Pending->Context;
    if (Context.IsShutdownRequested())
        return true;

    const auto MeshHandle = Pending->MeshRef.GetHandle();
    if (IsDependencyFailed(Context, MeshHandle)) {
        auto Error = Context.GetError(MeshHandle).value_or(ErrorMessage("ResourceMesh dependency became unavailable"));
        PublishResourceFailed<ResourceBottomLevelAccelerationStructure>(
            Context, Pending->Generation, Pending->Key, Error.Append("Failed to resolve BLAS mesh dependency"));
        return true;
    }
    auto* MeshResource = Context.TryGetReady(MeshHandle);
    if (!MeshResource)
        return false;

    if (Pending->Request.GeometryPolicy != BottomLevelAccelerationStructureGeometryPolicy::AllGeometryRecords) {
        PublishResourceFailed<ResourceBottomLevelAccelerationStructure>(
            Context, Pending->Generation, Pending->Key, ErrorMessage("Unsupported BLAS geometry policy"));
        return true;
    }

    // Get geometry records from GeometryManager using mesh file path
    auto& GeometryMgr = GeometryManager::Get();
    auto GeometryRecords = GeometryMgr.FindGeometryRecords(MeshHandle.GetKey());
    if (GeometryRecords.empty()) {
        PublishResourceFailed<ResourceBottomLevelAccelerationStructure>(
            Context,
            Pending->Generation,
            Pending->Key,
            ErrorMessage("No geometry records found for mesh"));
        return true;
    }

    std::vector<RHIRef<RHIVertexBuffer>>                      PositionRefs;
    std::vector<RHIRef<RHIIndexBuffer>>                       IndexRefs;
    std::vector<RHITriangleAccelerationStructureGeometryDesc> Geometries;
    for (const auto& Record : GeometryRecords) {
        if (!Record.PositionBuffer || !Record.IndexBuffer || Record.IndexCount == 0) {
            PublishResourceFailed<ResourceBottomLevelAccelerationStructure>(
                Context,
                Pending->Generation,
                Pending->Key,
                ErrorMessage("GeometryRecord has no valid position/index geometry for BLAS"));
            return true;
        }

        auto  PositionRef    = Record.PositionBuffer;
        auto  IndexRef       = Record.IndexBuffer;
        auto* PositionBuffer = PositionRef.TryGet();
        auto* IndexBuffer    = IndexRef.TryGet();
        if (!PositionBuffer || !IndexBuffer) {
            PublishResourceFailed<ResourceBottomLevelAccelerationStructure>(
                Context,
                Pending->Generation,
                Pending->Key,
                ErrorMessage("BLAS geometry buffer dependency became unavailable"));
            return true;
        }

        Geometries.push_back(RHITriangleAccelerationStructureGeometryDesc{
            .VertexBufferRef = PositionRef,
            .IndexBufferRef  = IndexRef,
        });
        PositionRefs.push_back(std::move(PositionRef));
        IndexRefs.push_back(std::move(IndexRef));
    }

    if (Geometries.empty()) {
        PublishResourceFailed<ResourceBottomLevelAccelerationStructure>(
            Context,
            Pending->Generation,
            Pending->Key,
            ErrorMessage("Mesh contains no triangle geometry for BLAS"));
        return true;
    }

    if (!MarkResourceRhiCommitting<ResourceBottomLevelAccelerationStructure>(
            Context, Pending->Key, Pending->Generation))
        return true;

    auto Payload = RHIRenderDevice::Get().CreateBottomLevelAccelerationStructure(
        RHIBottomLevelAccelerationStructureDesc{.Geometries = std::move(Geometries)});
    if (!Payload) {
        PublishResourceFailed<ResourceBottomLevelAccelerationStructure>(
            Context, Pending->Generation, Pending->Key, Payload.error().Append("Failed to create BLAS RHI payload"));
        return true;
    }

    auto ResourceValue = std::make_unique<ResourceBottomLevelAccelerationStructure>(
        std::move(PositionRefs), std::move(IndexRefs), std::move(*Payload));
    PublishResourceReady<ResourceBottomLevelAccelerationStructure>(
        Context, Pending->Generation, Pending->Key, {.Object = std::move(ResourceValue)});
    return true;
}

} // namespace

[[nodiscard]] auto RequestBottomLevelAccelerationStructure(ResourceContext&                    Context,
                                                                 const ResourceHandle<ResourceMesh>& MeshHandle,
                                                                 const BottomLevelAccelerationStructureRequest& Request)
    -> ResourceHandle<ResourceBottomLevelAccelerationStructure> {
    const auto Key  = BuildBottomLevelAccelerationStructureKey(MeshHandle, Request);
    auto       Work = BeginResourceWork<ResourceBottomLevelAccelerationStructure>(Context, Key);
    if (!Work.ShouldStartWork)
        return Work.Handle;

    auto MeshRef = AcquireResourceRef(Context, MeshHandle);
    if (!MeshRef) {
        PublishResourceFailed<ResourceBottomLevelAccelerationStructure>(
            Context,
            Work.Handle.GetGeneration(),
            Key,
            ErrorMessage("BLAS request received an invalid mesh resource reference"));
        return Work.Handle;
    }

    auto Pending        = std::make_shared<PendingBottomLevelAccelerationStructureRequest>();
    Pending->Context    = &Context;
    Pending->Generation = Work.Handle.GetGeneration();
    Pending->Key        = Key;
    Pending->Request    = Request;
    Pending->MeshRef    = std::move(MeshRef);
    Context.EnqueueRhiDependencyWaiter(
        [Pending] { return WaitForBottomLevelAccelerationStructureDependencies(Pending); });
    return Work.Handle;
}

[[nodiscard]] auto RequestTopLevelAccelerationStructure(ResourceContext&                            Context,
                                                              StringView                                  ScopeKey,
                                                              const RHITopLevelAccelerationStructureDesc& Desc)
    -> ResourceHandle<ResourceTopLevelAccelerationStructure> {
    const auto Key  = BuildTopLevelAccelerationStructureKey(ScopeKey);
    auto       Work = BeginResourceWork<ResourceTopLevelAccelerationStructure>(Context, Key);
    if (!Work.ShouldStartWork)
        return Work.Handle;
    if (ScopeKey.empty()) {
        PublishResourceFailed<ResourceTopLevelAccelerationStructure>(
            Context,
            Work.Handle.GetGeneration(),
            Key,
            ErrorMessage("TLAS request requires a non-empty renderer scope key"));
        return Work.Handle;
    }

    auto* ContextPtr = &Context;
    auto  EnqueueResult =
        TaskGraph::Get().Enqueue(ThreadQueue::RHI, [ContextPtr, Generation = Work.Handle.GetGeneration(), Key, Desc] {
            auto& Context = *ContextPtr;
            if (Context.IsShutdownRequested())
                return;
            if (!MarkResourceRhiCommitting<ResourceTopLevelAccelerationStructure>(Context, Key, Generation))
                return;

            auto Payload = RHIRenderDevice::Get().CreateTopLevelAccelerationStructure(Desc);
            if (!Payload) {
                PublishResourceFailed<ResourceTopLevelAccelerationStructure>(
                    Context, Generation, Key, Payload.error().Append("Failed to create TLAS RHI payload"));
                return;
            }

            auto ResourceValue = std::make_unique<ResourceTopLevelAccelerationStructure>(std::move(*Payload));
            PublishResourceReady<ResourceTopLevelAccelerationStructure>(
                Context, Generation, Key, {.Object = std::move(ResourceValue)});
        });
    if (!EnqueueResult) {
        PublishResourceFailed<ResourceTopLevelAccelerationStructure>(
            Context,
            Work.Handle.GetGeneration(),
            Key,
            EnqueueResult.error().Append(Format("Failed to enqueue async {} work '{}'",
                                                ResourceTraits<ResourceTopLevelAccelerationStructure>::Info.Label,
                                                Key)));
    }
    return Work.Handle;
}

} // namespace SoulEngine
