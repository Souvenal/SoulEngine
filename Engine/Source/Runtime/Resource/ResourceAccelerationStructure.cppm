module;

#include <hlsl++.h>

export module Resource:AccelerationStructure;

export import Core;
export import RHI;
import :Context;
import :RequestCommon;
export import :Types;
import TaskGraph;
export import std;

namespace SoulEngine {
namespace {

[[nodiscard]] auto BuildBottomLevelAccelerationStructureKey(StringView                                     MeshPath,
                                                            const BottomLevelAccelerationStructureRequest& Request)
    -> String {
    return Format("rt-blas/mesh={}/policy={}", MeshPath, static_cast<Uint32>(Request.GeometryPolicy));
}

[[nodiscard]] auto BuildTopLevelAccelerationStructureKey(StringView ScopeKey) -> String {
    return Format("rt-tlas/scope={}", ScopeKey);
}

struct PendingBottomLevelAccelerationStructureRequest {
    ResourceContext*                                          Context    = nullptr;
    ResourceGeneration                                        Generation = 0;
    String                                                    Key        = {};
    BottomLevelAccelerationStructureRequest                   Request    = {};
    std::vector<RHITriangleAccelerationStructureGeometryDesc> Geometries = {};
};

[[nodiscard]] auto
WaitForBottomLevelAccelerationStructureDependencies(const SPtr<PendingBottomLevelAccelerationStructureRequest>& Pending)
    -> bool {
    auto& Context = *Pending->Context;
    if (Context.IsShutdownRequested())
        return true;

    if (Pending->Request.GeometryPolicy != BottomLevelAccelerationStructureGeometryPolicy::AllGeometryRecords) {
        PublishResourceFailed<ResourceBottomLevelAccelerationStructure>(
            Context, Pending->Generation, Pending->Key, ErrorMessage("Unsupported BLAS geometry policy"));
        return true;
    }

    if (Pending->Geometries.empty()) {
        PublishResourceFailed<ResourceBottomLevelAccelerationStructure>(
            Context, Pending->Generation, Pending->Key, ErrorMessage("No geometry records found for mesh"));
        return true;
    }

    std::vector<RHIRef<RHIVertexBuffer>>                      PositionRefs;
    std::vector<RHIRef<RHIIndexBuffer>>                       IndexRefs;
    std::vector<RHITriangleAccelerationStructureGeometryDesc> Geometries;
    for (const auto& Geometry : Pending->Geometries) {
        if (!Geometry.VertexBufferRef || !Geometry.IndexBufferRef)
            return false;

        auto  PositionRef    = Geometry.VertexBufferRef;
        auto  IndexRef       = Geometry.IndexBufferRef;
        auto* PositionBuffer = PositionRef.TryGet();
        auto* IndexBuffer    = IndexRef.TryGet();
        if (!PositionBuffer || !IndexBuffer)
            return false;
        if (IndexBuffer->GetIndexCount() == 0) {
            PublishResourceFailed<ResourceBottomLevelAccelerationStructure>(
                Context,
                Pending->Generation,
                Pending->Key,
                ErrorMessage("RHI geometry has no valid index data for BLAS"));
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
            Context, Pending->Generation, Pending->Key, ErrorMessage("Mesh contains no triangle geometry for BLAS"));
        return true;
    }

    if (!MarkResourceRhiCommitting<ResourceBottomLevelAccelerationStructure>(
            Context, Pending->Key, Pending->Generation))
        return true;

    auto Payload = RHIRenderDevice::Get().CreateBottomLevelAccelerationStructure(
        Format("BLAS/{}", Pending->Key), RHIBottomLevelAccelerationStructureDesc{.Geometries = std::move(Geometries)});
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

[[nodiscard]] auto
RequestBottomLevelAccelerationStructure(ResourceContext&                                                 Context,
                                        StringView                                                       MeshPath,
                                        const std::vector<RHITriangleAccelerationStructureGeometryDesc>& Geometries,
                                        const BottomLevelAccelerationStructureRequest&                   Request)
    -> ResourceHandle<ResourceBottomLevelAccelerationStructure> {
    const auto Key  = BuildBottomLevelAccelerationStructureKey(MeshPath, Request);
    auto       Work = BeginResourceWork<ResourceBottomLevelAccelerationStructure>(Context, Key);
    if (!Work.ShouldStartWork)
        return Work.Handle;

    auto Pending        = std::make_shared<PendingBottomLevelAccelerationStructureRequest>();
    Pending->Context    = &Context;
    Pending->Generation = Work.Handle.GetGeneration();
    Pending->Key        = Key;
    Pending->Request    = Request;
    Pending->Geometries = Geometries;
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
        TaskGraph::Get().EnqueueTask(ThreadQueue::RHI, [ContextPtr, Generation = Work.Handle.GetGeneration(), Key, Desc] {
            auto& Context = *ContextPtr;
            if (Context.IsShutdownRequested())
                return;
            if (!MarkResourceRhiCommitting<ResourceTopLevelAccelerationStructure>(Context, Key, Generation))
                return;

            auto Payload = RHIRenderDevice::Get().CreateTopLevelAccelerationStructure(Format("TLAS/{}", Key), Desc);
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
