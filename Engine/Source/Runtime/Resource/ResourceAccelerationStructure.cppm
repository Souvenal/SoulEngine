export module Resource:AccelerationStructure;

export import Core;
export import RHI;
import :Context;
import :RequestCommon;
export import :Types;
import TaskGraph;
export import std;

using namespace SoulEngine::Core;

namespace SoulEngine::Resource {
namespace {

[[nodiscard]] auto BuildBottomLevelAccelerationStructureKey(const ResourceHandle<Mesh>& MeshHandle,
                                                             const BottomLevelAccelerationStructureRequest& Request)
    -> String {
    return Format("rt-blas/mesh={}/generation={}/flags={}/policy={}",
                  MeshHandle.GetKey(),
                  MeshHandle.GetGeneration(),
                  static_cast<Uint32>(Request.BuildFlags),
                  static_cast<Uint32>(Request.GeometryPolicy));
}

[[nodiscard]] auto BuildTopLevelAccelerationStructureKey(StringView ScopeKey,
                                                          const RHI::TopLevelAccelerationStructureDesc& Desc) -> String {
    return Format("rt-tlas/scope={}/flags={}", ScopeKey, static_cast<Uint32>(Desc.BuildFlags));
}

struct PendingBottomLevelAccelerationStructureRequest {
    ResourceContext*                               Context = nullptr;
    ResourceGeneration                             Generation = 0;
    String                                         Key = {};
    BottomLevelAccelerationStructureRequest        Request = {};
    ResourceRef<Mesh>                              MeshRef = {};
};

[[nodiscard]] auto IsDependencyFailed(ResourceContext& Context, const ResourceHandle<Mesh>& Handle) -> bool {
    const auto State = Context.GetState(Handle);
    return State == ResourceState::Failed || State == ResourceState::Stale || State == ResourceState::Unknown;
}

[[nodiscard]] auto WaitForBottomLevelAccelerationStructureDependencies(
    const SPtr<PendingBottomLevelAccelerationStructureRequest>& Pending) -> bool {
    auto& Context = *Pending->Context;
    if (Context.IsShutdownRequested())
        return true;

    const auto MeshHandle = Pending->MeshRef.GetHandle();
    if (IsDependencyFailed(Context, MeshHandle)) {
        auto Error = Context.GetError(MeshHandle).value_or(ErrorMessage("Mesh dependency became unavailable"));
        PublishResourceFailed<BottomLevelAccelerationStructure>(
            Context, Pending->Generation, Pending->Key, Error.Append("Failed to resolve BLAS mesh dependency"));
        return true;
    }
    auto* MeshResource = Context.TryGetReady(MeshHandle);
    if (!MeshResource)
        return false;

    if (Pending->Request.GeometryPolicy != BottomLevelAccelerationStructureGeometryPolicy::AllMeshSubMeshes) {
        PublishResourceFailed<BottomLevelAccelerationStructure>(
            Context,
            Pending->Generation,
            Pending->Key,
            ErrorMessage("Unsupported BLAS geometry policy"));
        return true;
    }

    std::vector<ResourceHandle<RHI::VertexBuffer>> PositionHandles;
    std::vector<ResourceHandle<RHI::IndexBuffer>>  IndexHandles;
    std::vector<RHI::TriangleAccelerationStructureGeometryDesc> Geometries;
    for (const auto& Group : MeshResource->GetMeshGroups()) {
        for (const auto& SubMesh : Group.SubMeshes) {
            if (!SubMesh.PositionVB.IsValid() || !SubMesh.IB.IsValid() || SubMesh.VertexCount == 0 || SubMesh.Indices.empty()) {
                PublishResourceFailed<BottomLevelAccelerationStructure>(
                    Context,
                    Pending->Generation,
                    Pending->Key,
                    ErrorMessage("Mesh submesh has no valid position/index geometry for BLAS"));
                return true;
            }

            const auto PositionState = Context.GetState(SubMesh.PositionVB);
            const auto IndexState = Context.GetState(SubMesh.IB);
            if (PositionState == ResourceState::Failed || PositionState == ResourceState::Stale ||
                IndexState == ResourceState::Failed || IndexState == ResourceState::Stale) {
                PublishResourceFailed<BottomLevelAccelerationStructure>(
                    Context,
                    Pending->Generation,
                    Pending->Key,
                    ErrorMessage("BLAS geometry buffer dependency failed"));
                return true;
            }
            auto* PositionBuffer = Context.TryGetReady(SubMesh.PositionVB);
            auto* IndexBuffer = Context.TryGetReady(SubMesh.IB);
            if (!PositionBuffer || !IndexBuffer)
                return false;

            PositionHandles.push_back(SubMesh.PositionVB);
            IndexHandles.push_back(SubMesh.IB);
            Geometries.push_back(RHI::TriangleAccelerationStructureGeometryDesc{
                .VertexBufferPtr = PositionBuffer,
                .VertexCount = SubMesh.VertexCount,
                .VertexStride = sizeof(hlslpp::interop::float3),
                .VertexFormat = RHI::Format::R32G32B32_SFLOAT,
                .IndexBufferPtr = IndexBuffer,
                .IndexCount = static_cast<Uint32>(SubMesh.Indices.size()),
            });
        }
    }

    if (Geometries.empty()) {
        PublishResourceFailed<BottomLevelAccelerationStructure>(
            Context,
            Pending->Generation,
            Pending->Key,
            ErrorMessage("Mesh contains no triangle geometry for BLAS"));
        return true;
    }

    std::vector<ResourceRef<RHI::VertexBuffer>> PositionRefs;
    std::vector<ResourceRef<RHI::IndexBuffer>> IndexRefs;
    PositionRefs.reserve(PositionHandles.size());
    IndexRefs.reserve(IndexHandles.size());
    for (Uint32 GeometryIndex = 0; GeometryIndex < Geometries.size(); ++GeometryIndex) {
        auto PositionRef = AcquireResourceRef(Context, PositionHandles[GeometryIndex]);
        auto IndexRef = AcquireResourceRef(Context, IndexHandles[GeometryIndex]);
        if (!PositionRef || !IndexRef) {
            PublishResourceFailed<BottomLevelAccelerationStructure>(
                Context,
                Pending->Generation,
                Pending->Key,
                ErrorMessage("BLAS geometry buffer dependency became stale before ownership could be retained"));
            return true;
        }
        PositionRefs.push_back(std::move(PositionRef));
        IndexRefs.push_back(std::move(IndexRef));
    }

    if (!MarkResourceRhiCommitting<BottomLevelAccelerationStructure>(Context, Pending->Key, Pending->Generation))
        return true;

    auto Payload = RHI::RenderDevice::Get().CreateBottomLevelAccelerationStructure(RHI::BottomLevelAccelerationStructureDesc{
        .Geometries = std::move(Geometries),
        .BuildFlags = Pending->Request.BuildFlags,
    });
    if (!Payload) {
        PublishResourceFailed<BottomLevelAccelerationStructure>(
            Context,
            Pending->Generation,
            Pending->Key,
            Payload.error().Append("Failed to create BLAS RHI payload"));
        return true;
    }

    auto ResourceValue = std::make_unique<BottomLevelAccelerationStructure>(
        std::move(PositionRefs), std::move(IndexRefs), std::move(*Payload));
    PublishResourceReady<BottomLevelAccelerationStructure>(
        Context, Pending->Generation, Pending->Key, {.Object = std::move(ResourceValue)});
    return true;
}

} // namespace

[[nodiscard]] auto SubmitBottomLevelAccelerationStructureRequest(
    ResourceContext& Context,
    const ResourceHandle<Mesh>& MeshHandle,
    const BottomLevelAccelerationStructureRequest& Request) -> ResourceHandle<BottomLevelAccelerationStructure> {
    const auto Key = BuildBottomLevelAccelerationStructureKey(MeshHandle, Request);
    auto Work = BeginResourceWork<BottomLevelAccelerationStructure>(Context, Key);
    if (!Work.ShouldStartWork)
        return Work.Handle;

    auto MeshRef = AcquireResourceRef(Context, MeshHandle);
    if (!MeshRef) {
        PublishResourceFailed<BottomLevelAccelerationStructure>(
            Context,
            Work.Handle.GetGeneration(),
            Key,
            ErrorMessage("BLAS request received an invalid mesh resource reference"));
        return Work.Handle;
    }

    auto Pending = std::make_shared<PendingBottomLevelAccelerationStructureRequest>();
    Pending->Context = &Context;
    Pending->Generation = Work.Handle.GetGeneration();
    Pending->Key = Key;
    Pending->Request = Request;
    Pending->MeshRef = std::move(MeshRef);
    Context.EnqueueRhiDependencyWaiter([Pending] {
        return WaitForBottomLevelAccelerationStructureDependencies(Pending);
    });
    return Work.Handle;
}

[[nodiscard]] auto SubmitTopLevelAccelerationStructureRequest(ResourceContext& Context,
                                                               StringView ScopeKey,
                                                               const RHI::TopLevelAccelerationStructureDesc& Desc)
    -> ResourceHandle<TopLevelAccelerationStructure> {
    const auto Key = BuildTopLevelAccelerationStructureKey(ScopeKey, Desc);
    auto Work = BeginResourceWork<TopLevelAccelerationStructure>(Context, Key);
    if (!Work.ShouldStartWork)
        return Work.Handle;
    if (ScopeKey.empty()) {
        PublishResourceFailed<TopLevelAccelerationStructure>(
            Context,
            Work.Handle.GetGeneration(),
            Key,
            ErrorMessage("TLAS request requires a non-empty renderer scope key"));
        return Work.Handle;
    }

    auto* ContextPtr = &Context;
    Work.Graph->Enqueue(ThreadQueue::RHI, [ContextPtr, Generation = Work.Handle.GetGeneration(), Key, Desc] {
        auto& Context = *ContextPtr;
        if (Context.IsShutdownRequested())
            return;
        if (!MarkResourceRhiCommitting<TopLevelAccelerationStructure>(Context, Key, Generation))
            return;

        auto Payload = RHI::RenderDevice::Get().CreateTopLevelAccelerationStructure(Desc);
        if (!Payload) {
            PublishResourceFailed<TopLevelAccelerationStructure>(
                Context,
                Generation,
                Key,
                Payload.error().Append("Failed to create TLAS RHI payload"));
            return;
        }

        auto ResourceValue = std::make_unique<TopLevelAccelerationStructure>(std::move(*Payload));
        PublishResourceReady<TopLevelAccelerationStructure>(
            Context, Generation, Key, {.Object = std::move(ResourceValue)});
    });
    return Work.Handle;
}

} // namespace SoulEngine::Resource
