module;

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <hlsl++.h>

export module Resource:Mesh;

export import Core;
export import RHI;
import :Context;
import :RequestCommon;
export import :Types;
import :Material;
import TaskGraph;
export import std;

namespace SoulEngine {

[[nodiscard]] auto NormalizeMeshResourcePath(StringView InPath) -> String {
    return Path(String(InPath)).lexically_normal().string();
}

[[nodiscard]] auto ParseAssimpMeshes(StringView MeshPath, ResourceMesh& Out) -> std::expected<void, ErrorMessage> {
    Assimp::Importer Importer;

    constexpr Uint32 Flags = aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_CalcTangentSpace |
                             aiProcess_GenSmoothNormals | aiProcess_JoinIdenticalVertices |
                             aiProcess_ImproveCacheLocality | aiProcess_OptimizeMeshes;

    const auto* Scene = Importer.ReadFile(String(MeshPath).c_str(), Flags);
    if (!Scene || !Scene->mRootNode || Scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)
        return std::unexpected(
            ErrorMessage(Format("Assimp import failed '{}': {}", MeshPath, Importer.GetErrorString())));

    Out.m_Name               = Path(String(MeshPath)).stem().string();
    const Path MeshDirectory = Path(String(MeshPath)).parent_path();
    // Register all materials from the scene
    MaterialManager::Get().RegisterMaterialsFromScene(Scene, MeshDirectory);
    if (!Scene->HasMeshes())
        return {};

    Out.m_MeshGroups.reserve(Scene->mNumMeshes);
    for (Uint32 MeshIndex = 0; MeshIndex < Scene->mNumMeshes; ++MeshIndex) {
        const auto* AiMesh = Scene->mMeshes[MeshIndex];
        if (!AiMesh || !AiMesh->mNumVertices || !AiMesh->HasFaces())
            continue;

        MeshGroup    Group{.Name = AiMesh->mName.C_Str()};
        const Uint32 MaterialSlot = AiMesh->mMaterialIndex;

        // Look up material ID by name from the aiScene material.
        aiString AiName;
        Scene->mMaterials[MaterialSlot]->Get(AI_MATKEY_NAME, AiName);
        const auto SubMaterialId = MaterialManager::Get().FindMaterialId(AiName.C_Str());

        SubMesh Sub{
            .VertexCount = AiMesh->mNumVertices,
            .MaterialId  = SubMaterialId,
            .HasUV0      = AiMesh->mTextureCoords[0] != nullptr,
            .HasTangents = AiMesh->mTangents != nullptr && AiMesh->mBitangents != nullptr,
        };
        Sub.Positions.reserve(AiMesh->mNumVertices);
        Sub.Normals.reserve(AiMesh->mNumVertices);
        Sub.Tangents.reserve(AiMesh->mNumVertices);
        Sub.UVs.reserve(AiMesh->mNumVertices);

        for (Uint32 VertexIndex = 0; VertexIndex < AiMesh->mNumVertices; ++VertexIndex) {
            const auto& Position = AiMesh->mVertices[VertexIndex];
            Sub.Positions.push_back(hlslpp::interop::float3{hlslpp::float3{Position.x, Position.y, Position.z}});
            const auto Normal = AiMesh->mNormals ? AiMesh->mNormals[VertexIndex] : aiVector3D{0.0f, 1.0f, 0.0f};
            Sub.Normals.push_back(hlslpp::interop::float3{hlslpp::float3{Normal.x, Normal.y, Normal.z}});

            const auto Tangent    = AiMesh->mTangents ? AiMesh->mTangents[VertexIndex] : aiVector3D{1.0f, 0.0f, 0.0f};
            float      Handedness = 1.0f;
            if (Sub.HasTangents) {
                const auto Bitangent = AiMesh->mBitangents[VertexIndex];
                const auto Cross     = Normal ^ Tangent;
                Handedness           = (Cross * Bitangent) < 0.0f ? -1.0f : 1.0f;
            }
            Sub.Tangents.push_back(
                hlslpp::interop::float4{hlslpp::float4{Tangent.x, Tangent.y, Tangent.z, Handedness}});

            const auto UV = Sub.HasUV0 ? AiMesh->mTextureCoords[0][VertexIndex] : aiVector3D{};
            Sub.UVs.push_back(hlslpp::interop::float2{hlslpp::float2{UV.x, UV.y}});
        }

        Sub.Indices.reserve(AiMesh->mNumFaces * 3);
        for (Uint32 FaceIndex = 0; FaceIndex < AiMesh->mNumFaces; ++FaceIndex) {
            const auto& Face = AiMesh->mFaces[FaceIndex];
            if (Face.mNumIndices != 3)
                return std::unexpected(ErrorMessage(Format("Assimp imported non-triangle face in '{}'", MeshPath)));
            Sub.Indices.insert(Sub.Indices.end(), Face.mIndices, Face.mIndices + Face.mNumIndices);
        }
        if (Sub.Indices.empty())
            continue;
        Group.SubMeshes.emplace_back(std::move(Sub));
        Out.m_MeshGroups.emplace_back(std::move(Group));
    }
    return {};
}

[[nodiscard]] auto ResourceMesh::GetMeshGroups() -> std::vector<MeshGroup>& {
    return m_MeshGroups;
}

[[nodiscard]] auto ResourceMesh::GetMeshGroups() const -> const std::vector<MeshGroup>& {
    return m_MeshGroups;
}

struct MeshBufferRequestHandles {
    RHIRef<RHIVertexBuffer> Position = nullptr;
    RHIRef<RHIVertexBuffer> Normal   = nullptr;
    RHIRef<RHIVertexBuffer> Tangent  = nullptr;
    RHIRef<RHIVertexBuffer> UV       = nullptr;
    RHIRef<RHIIndexBuffer>  Index    = nullptr;
};

struct PendingMeshUpload {
    ResourceContext*                      Context       = nullptr;
    ResourceGeneration                    Generation    = 0;
    String                                Key           = {};
    UPtr<ResourceMesh>                    Mesh          = nullptr;
    std::vector<MeshBufferRequestHandles> BufferHandles = {};
};

[[nodiscard]] auto RequestMeshVertexBuffer(const RHIVertexBufferDesc& Desc) -> RHIRef<RHIVertexBuffer> {
    auto Buffer = RHIRenderDevice::Get().CreateVertexBuffer(Desc);
    if (!Buffer) {
        LogError("Failed to queue mesh vertex buffer creation: {}", Buffer.error().ToString());
        return {};
    }
    return std::move(*Buffer);
}

[[nodiscard]] auto RequestMeshIndexBuffer(const RHIIndexBufferDesc& Desc) -> RHIRef<RHIIndexBuffer> {
    auto Buffer = RHIRenderDevice::Get().CreateIndexBuffer(Desc);
    if (!Buffer) {
        LogError("Failed to queue mesh index buffer creation: {}", Buffer.error().ToString());
        return {};
    }
    return std::move(*Buffer);
}

[[nodiscard]] auto RequestMeshBuffers(const ResourceMesh& Mesh) -> std::vector<MeshBufferRequestHandles> {
    std::vector<MeshBufferRequestHandles> Result = {};
    const auto&                           Groups = Mesh.GetMeshGroups();
    for (const auto& Group : Groups) {
        for (const auto& Sub : Group.SubMeshes) {
            Result.emplace_back(MeshBufferRequestHandles{
                .Position = RequestMeshVertexBuffer({.Data        = std::as_bytes(std::span{Sub.Positions}),
                                                     .VertexCount = Sub.Positions.size(),
                                                     .Stride      = sizeof(hlslpp::interop::float3)}),
                .Normal   = RequestMeshVertexBuffer({.Data        = std::as_bytes(std::span{Sub.Normals}),
                                                     .VertexCount = Sub.Normals.size(),
                                                     .Stride      = sizeof(hlslpp::interop::float3)}),
                .Tangent  = RequestMeshVertexBuffer({.Data        = std::as_bytes(std::span{Sub.Tangents}),
                                                     .VertexCount = Sub.Tangents.size(),
                                                     .Stride      = sizeof(hlslpp::interop::float4)}),
                .UV       = RequestMeshVertexBuffer({.Data        = std::as_bytes(std::span{Sub.UVs}),
                                                     .VertexCount = Sub.UVs.size(),
                                                     .Stride      = sizeof(hlslpp::interop::float2)}),
                .Index    = RequestMeshIndexBuffer(
                    {.Data = std::as_bytes(std::span{Sub.Indices}), .IndexCount = Sub.Indices.size()}),
            });
        }
    }
    return Result;
}

[[nodiscard]] auto ResolveMeshBufferDependencies(const std::shared_ptr<PendingMeshUpload>& Pending) -> bool {
    auto& Context = *Pending->Context;
    if (Context.IsShutdownRequested())
        return true;

    std::size_t BufferIndex = 0;
    for (auto& Group : Pending->Mesh->GetMeshGroups()) {
        for (auto& Sub : Group.SubMeshes) {
            if (BufferIndex >= Pending->BufferHandles.size()) {
                PublishResourceFailed<ResourceMesh>(
                    Context, Pending->Generation, Pending->Key, ErrorMessage("Mesh buffer dependency count mismatch"));
                return true;
            }

            const auto& Handles  = Pending->BufferHandles[BufferIndex++];
            auto        Position = Handles.Position;
            auto        Normal   = Handles.Normal;
            auto        Tangent  = Handles.Tangent;
            auto        UV       = Handles.UV;
            auto        Index    = Handles.Index;
            if (Position.GetState() == RHIRefState::Failed || Normal.GetState() == RHIRefState::Failed ||
                Tangent.GetState() == RHIRefState::Failed || UV.GetState() == RHIRefState::Failed ||
                Index.GetState() == RHIRefState::Failed) {
                PublishResourceFailed<ResourceMesh>(
                    Context, Pending->Generation, Pending->Key, ErrorMessage("Mesh buffer creation failed"));
                return true;
            }
            if (!Position.TryGet() || !Normal.TryGet() || !Tangent.TryGet() || !UV.TryGet() || !Index.TryGet())
                return false;

            Sub.PositionVB = std::move(Position);
            Sub.NormalVB   = std::move(Normal);
            Sub.TangentVB  = std::move(Tangent);
            Sub.UVVB       = std::move(UV);
            Sub.IB         = std::move(Index);
        }
    }

    if (BufferIndex != Pending->BufferHandles.size()) {
        PublishResourceFailed<ResourceMesh>(
            Context, Pending->Generation, Pending->Key, ErrorMessage("Mesh buffer dependency count mismatch"));
        return true;
    }

    PublishResourceReady<ResourceMesh>(
        Context, Pending->Generation, Pending->Key, {.Object = std::move(Pending->Mesh)});
    return true;
}

[[nodiscard]] auto RequestMesh(ResourceContext& Context, StringView MeshPath) -> ResourceHandle<ResourceMesh> {
    const auto Key  = NormalizeMeshResourcePath(MeshPath);
    auto       Work = BeginResourceWork<ResourceMesh>(Context, Key);
    if (!Work.ShouldStartWork)
        return Work.Handle;

    auto* ContextPtr = &Context;
    auto  EnqueueResult =
        TaskGraph::Get().EnqueueBackground([ContextPtr, Generation = Work.Handle.GetGeneration(), Key] {
            auto& Context = *ContextPtr;
            if (Context.IsShutdownRequested())
                return;

            auto ImportedMesh = std::make_unique<ResourceMesh>();
            if (auto Parsed = ParseAssimpMeshes(Key, *ImportedMesh); !Parsed) {
                PublishResourceFailed<ResourceMesh>(Context, Generation, Key, Parsed.error());
                return;
            }
            if (Context.IsShutdownRequested())
                return;

            auto Pending           = std::make_shared<PendingMeshUpload>();
            Pending->Context       = &Context;
            Pending->Generation    = Generation;
            Pending->Key           = Key;
            Pending->BufferHandles = RequestMeshBuffers(*ImportedMesh);
            Pending->Mesh          = std::move(ImportedMesh);
            Context.EnqueueRhiDependencyWaiter([Pending] { return ResolveMeshBufferDependencies(Pending); });
        });
    if (!EnqueueResult) {
        PublishResourceFailed<ResourceMesh>(
            Context,
            Work.Handle.GetGeneration(),
            Key,
            EnqueueResult.error().Append(
                Format("Failed to enqueue async {} work '{}'", ResourceTraits<ResourceMesh>::Info.Label, Key)));
    }

    return Work.Handle;
}

} // namespace SoulEngine
