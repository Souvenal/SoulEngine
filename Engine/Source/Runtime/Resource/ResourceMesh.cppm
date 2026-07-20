module;

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <hlsl++.h>

export module Resource:Mesh;

export import Core;
export import RHI;
import :Buffer;
import :Context;
import :RequestCommon;
export import :Types;
import TaskGraph;
export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Resource {

[[nodiscard]] auto NormalizeMeshResourcePath(StringView InPath) -> String {
    return Path(String(InPath)).lexically_normal().string();
}

[[nodiscard]] auto ParseAssimpMeshes(StringView MeshPath, Mesh& Out) -> std::expected<void, ErrorMessage> {
    Assimp::Importer Importer;

    constexpr Uint32 Flags = aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_CalcTangentSpace |
                             aiProcess_GenSmoothNormals | aiProcess_JoinIdenticalVertices | aiProcess_ImproveCacheLocality |
                             aiProcess_OptimizeMeshes;

    const auto* Scene = Importer.ReadFile(String(MeshPath).c_str(), Flags);
    if (!Scene || !Scene->mRootNode || Scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)
        return std::unexpected(
            ErrorMessage(Format("Assimp import failed '{}': {}", MeshPath, Importer.GetErrorString())));

    Out.m_Name = Path(String(MeshPath)).stem().string();
    if (!Scene->HasMeshes())
        return {};

    Out.m_MeshGroups.reserve(Scene->mNumMeshes);
    for (Uint32 MeshIndex = 0; MeshIndex < Scene->mNumMeshes; ++MeshIndex) {
        const auto* AiMesh = Scene->mMeshes[MeshIndex];
        if (!AiMesh || !AiMesh->mNumVertices || !AiMesh->HasFaces())
            continue;

        MeshGroup Group{
            .Name = AiMesh->mName.C_Str(),
        };
        SubMesh Sub{
            .VertexCount  = AiMesh->mNumVertices,
            .MaterialSlot = AiMesh->mMaterialIndex,
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

            const auto Tangent = AiMesh->mTangents ? AiMesh->mTangents[VertexIndex] : aiVector3D{1.0f, 0.0f, 0.0f};
            Sub.Tangents.push_back(hlslpp::interop::float4{hlslpp::float4{Tangent.x, Tangent.y, Tangent.z, 1.0f}});

            const auto UV = AiMesh->mTextureCoords[0] ? AiMesh->mTextureCoords[0][VertexIndex] : aiVector3D{};
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

auto Mesh::PopulateDrawPackets(std::vector<DrawPacket>& Out) const -> void {
    for (const auto& Group : m_MeshGroups) {
        for (const auto& Sub : Group.SubMeshes) {
            if (Sub.Indices.empty() || !Sub.IB.IsValid())
                continue;

            Out.push_back(DrawPacket{
                .PositionVB = Sub.PositionVB,
                .NormalVB   = Sub.NormalVB,
                .TangentVB  = Sub.TangentVB,
                .UVVB       = Sub.UVVB,
                .IB         = Sub.IB,
                .IndexCount = static_cast<Uint32>(Sub.Indices.size()),
            });
        }
    }
}

[[nodiscard]] auto Mesh::GetMeshGroups() -> std::vector<MeshGroup>& {
    return m_MeshGroups;
}

[[nodiscard]] auto Mesh::GetMeshGroups() const -> const std::vector<MeshGroup>& {
    return m_MeshGroups;
}

auto UploadMeshBuffers(ResourceContext& Context, const String& MeshKey, Mesh& InMesh) -> void {
    auto& Groups = InMesh.GetMeshGroups();
    for (std::size_t GroupIndex = 0; GroupIndex < Groups.size(); ++GroupIndex) {
        auto& Group = Groups[GroupIndex];
        for (std::size_t SubMeshIndex = 0; SubMeshIndex < Group.SubMeshes.size(); ++SubMeshIndex) {
            auto& Sub = Group.SubMeshes[SubMeshIndex];
            auto  Key = Format("{}/group{}/submesh{}", MeshKey, GroupIndex, SubMeshIndex);

            Sub.PositionVB = SubmitVertexBufferRequest(Context, Key + "_pos", {.Data = Sub.Positions.data(),
                                                                                .VertexCount = Sub.Positions.size(),
                                                                                .Stride = sizeof(hlslpp::interop::float3)});
            Sub.NormalVB = SubmitVertexBufferRequest(Context, Key + "_nrm", {.Data = Sub.Normals.data(),
                                                                              .VertexCount = Sub.Normals.size(),
                                                                              .Stride = sizeof(hlslpp::interop::float3)});
            Sub.TangentVB = SubmitVertexBufferRequest(Context, Key + "_tan", {.Data = Sub.Tangents.data(),
                                                                               .VertexCount = Sub.Tangents.size(),
                                                                               .Stride = sizeof(hlslpp::interop::float4)});
            Sub.UVVB = SubmitVertexBufferRequest(Context, Key + "_uv", {.Data = Sub.UVs.data(),
                                                                         .VertexCount = Sub.UVs.size(),
                                                                         .Stride = sizeof(hlslpp::interop::float2)});
            Sub.IB = SubmitIndexBufferRequest(
                Context, Key + "_ib", {.Data = Sub.Indices.data(), .IndexCount = Sub.Indices.size()});
        }
    }
}

[[nodiscard]] auto SubmitMeshRequest(ResourceContext& Context, StringView MeshPath) -> ResourceHandle<Mesh> {
    const auto Key = NormalizeMeshResourcePath(MeshPath);
    auto       Work = BeginResourceWork<Mesh>(Context, Key);
    if (!Work.ShouldStartWork)
        return Work.Handle;

    auto* ContextPtr = &Context;
    Work.Graph->EnqueueBackground([ContextPtr, Generation = Work.Handle.GetGeneration(), Key] {
        auto& Context = *ContextPtr;
        if (Context.IsShutdownRequested())
            return;

        auto ImportedMesh = std::make_unique<Mesh>();
        if (auto Parsed = ParseAssimpMeshes(Key, *ImportedMesh); !Parsed) {
            PublishResourceFailed<Mesh>(Context, Generation, Key, Parsed.error());
            return;
        }
        if (Context.IsShutdownRequested())
            return;

        UploadMeshBuffers(Context, Key, *ImportedMesh);
        PublishResourceReady<Mesh>(Context, Generation, Key, {.Object = std::move(ImportedMesh)});
    });

    return Work.Handle;
}

} // namespace SoulEngine::Resource
