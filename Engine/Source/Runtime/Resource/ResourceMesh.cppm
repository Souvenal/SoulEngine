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
import TaskGraph;
export import std;

namespace SoulEngine {

[[nodiscard]] auto NormalizeMeshResourcePath(StringView InPath) -> String {
    return Path(String(InPath)).lexically_normal().string();
}

namespace {

enum class ImportedTextureSemantic : Uint8 {
    BaseColor,
    Normal,
    Metallic,
    Roughness,
    Occlusion,
    Emissive,
};

[[nodiscard]] auto TextureSemanticName(ImportedTextureSemantic Semantic) -> StringView {
    switch (Semantic) {
    case ImportedTextureSemantic::BaseColor:
        return "base-color";
    case ImportedTextureSemantic::Normal:
        return "normal";
    case ImportedTextureSemantic::Metallic:
        return "metallic";
    case ImportedTextureSemantic::Roughness:
        return "roughness";
    case ImportedTextureSemantic::Occlusion:
        return "occlusion";
    case ImportedTextureSemantic::Emissive:
        return "emissive";
    }
    return "unknown";
}

[[nodiscard]] auto GetExternalTexturePath(const aiMaterial&       Material,
                                          aiTextureType           Type,
                                          ImportedTextureSemantic Semantic,
                                          const Path&             MeshDirectory) -> String {
    const auto TextureCount = Material.GetTextureCount(Type);
    if (TextureCount == 0)
        return {};
    if (TextureCount != 1) {
        LogWarning("Assimp material {} texture disabled: layered texture stacks are not supported",
                   TextureSemanticName(Semantic));
        return {};
    }

    aiString         TexturePath = {};
    aiTextureMapping Mapping     = aiTextureMapping_UV;
    Uint32           UVIndex     = 0;
    if (Material.GetTexture(Type, 0, &TexturePath, &Mapping, &UVIndex) != AI_SUCCESS) {
        LogWarning("Assimp material {} texture disabled: failed to read texture binding",
                   TextureSemanticName(Semantic));
        return {};
    }
    if (Mapping != aiTextureMapping_UV || UVIndex != 0) {
        LogWarning("Assimp material {} texture disabled: only UV0 mapping is supported", TextureSemanticName(Semantic));
        return {};
    }

    const Path SourcePath = TexturePath.C_Str();
    if (TexturePath.length == 0 || TexturePath.C_Str()[0] == '*') {
        LogWarning("Assimp material {} texture disabled: embedded textures are not supported",
                   TextureSemanticName(Semantic));
        return {};
    }
    if (SourcePath.is_absolute()) {
        LogWarning("Assimp material {} texture disabled: absolute paths are not supported",
                   TextureSemanticName(Semantic));
        return {};
    }

    const auto Normalized = (MeshDirectory / SourcePath).lexically_normal();
    if (Normalized.empty()) {
        LogWarning("Assimp material {} texture disabled: path could not be normalized", TextureSemanticName(Semantic));
        return {};
    }
    return Normalized.string();
}

[[nodiscard]] auto
ReadColor(const aiMaterial& Material, const char* Key, Uint32 Type, Uint32 Index, hlslpp::float3 Fallback)
    -> hlslpp::float3 {
    aiColor4D Value = {};
    if (Material.Get(Key, Type, Index, Value) != AI_SUCCESS)
        return Fallback;
    return hlslpp::float3{Value.r, Value.g, Value.b};
}

[[nodiscard]] auto ReadFloat(const aiMaterial& Material, const char* Key, Uint32 Type, Uint32 Index, float Fallback)
    -> float {
    float Value = Fallback;
    return Material.Get(Key, Type, Index, Value) == AI_SUCCESS ? Value : Fallback;
}

[[nodiscard]] auto ImportMaterial(const aiMaterial* AiMaterial, const Path& MeshDirectory, Uint32 MaterialIndex)
    -> ImportedPbrMaterial {
    ImportedPbrMaterial Result{.Name = Format("Material{}", MaterialIndex)};
    if (!AiMaterial)
        return Result;

    aiString Name = {};
    if (AiMaterial->Get(AI_MATKEY_NAME, Name) == AI_SUCCESS && Name.length != 0)
        Result.Name = Name.C_Str();

    aiColor4D BaseColor = {};
    if (AiMaterial->Get(AI_MATKEY_BASE_COLOR, BaseColor) == AI_SUCCESS) {
        Result.Material.BaseColor = hlslpp::float3{BaseColor.r, BaseColor.g, BaseColor.b};
    } else {
        Result.Material.BaseColor = ReadColor(*AiMaterial, AI_MATKEY_COLOR_DIFFUSE, Result.Material.BaseColor);
    }
    Result.Material.Emissive = ReadColor(*AiMaterial, AI_MATKEY_COLOR_EMISSIVE, hlslpp::float3{0.0f, 0.0f, 0.0f});
    Result.Material.Metallic = std::clamp(ReadFloat(*AiMaterial, AI_MATKEY_METALLIC_FACTOR, 0.0f), 0.0f, 1.0f);
    Result.Material.Roughness =
        std::clamp(ReadFloat(*AiMaterial, AI_MATKEY_ROUGHNESS_FACTOR, Result.Material.Roughness), 0.0f, 1.0f);

    Result.Material.BaseColorTexture = GetExternalTexturePath(
        *AiMaterial, aiTextureType_BASE_COLOR, ImportedTextureSemantic::BaseColor, MeshDirectory);
    if (Result.Material.BaseColorTexture.empty())
        Result.Material.BaseColorTexture = GetExternalTexturePath(
            *AiMaterial, aiTextureType_DIFFUSE, ImportedTextureSemantic::BaseColor, MeshDirectory);
    Result.Material.NormalTexture = GetExternalTexturePath(
        *AiMaterial, aiTextureType_NORMAL_CAMERA, ImportedTextureSemantic::Normal, MeshDirectory);
    if (Result.Material.NormalTexture.empty())
        Result.Material.NormalTexture =
            GetExternalTexturePath(*AiMaterial, aiTextureType_NORMALS, ImportedTextureSemantic::Normal, MeshDirectory);
    Result.Material.MetallicTexture =
        GetExternalTexturePath(*AiMaterial, aiTextureType_METALNESS, ImportedTextureSemantic::Metallic, MeshDirectory);
    Result.Material.RoughnessTexture = GetExternalTexturePath(
        *AiMaterial, aiTextureType_DIFFUSE_ROUGHNESS, ImportedTextureSemantic::Roughness, MeshDirectory);
    Result.Material.OcclusionTexture = GetExternalTexturePath(
        *AiMaterial, aiTextureType_AMBIENT_OCCLUSION, ImportedTextureSemantic::Occlusion, MeshDirectory);
    Result.Material.EmissiveTexture = GetExternalTexturePath(
        *AiMaterial, aiTextureType_EMISSION_COLOR, ImportedTextureSemantic::Emissive, MeshDirectory);
    if (Result.Material.EmissiveTexture.empty())
        Result.Material.EmissiveTexture = GetExternalTexturePath(
            *AiMaterial, aiTextureType_EMISSIVE, ImportedTextureSemantic::Emissive, MeshDirectory);

    if (!Result.Material.MetallicTexture.empty() &&
        Result.Material.MetallicTexture == Result.Material.RoughnessTexture) {
        Result.Material.MetallicRoughnessTexture = Result.Material.MetallicTexture;
        Result.Material.MetallicTexture          = {};
        Result.Material.RoughnessTexture         = {};
    }
    return Result;
}

} // namespace

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
    Out.m_ImportedMaterials.reserve(Scene->mNumMaterials);
    for (Uint32 MaterialIndex = 0; MaterialIndex < Scene->mNumMaterials; ++MaterialIndex) {
        auto ImportedMaterial = ImportMaterial(Scene->mMaterials[MaterialIndex], MeshDirectory, MaterialIndex);
        LogDebug("Imported material '{}' from '{}'", ImportedMaterial.Name, MeshPath);
        Out.m_ImportedMaterials.emplace_back(std::move(ImportedMaterial));
    }
    if (!Scene->HasMeshes())
        return {};

    Out.m_MeshGroups.reserve(Scene->mNumMeshes);
    for (Uint32 MeshIndex = 0; MeshIndex < Scene->mNumMeshes; ++MeshIndex) {
        const auto* AiMesh = Scene->mMeshes[MeshIndex];
        if (!AiMesh || !AiMesh->mNumVertices || !AiMesh->HasFaces())
            continue;

        MeshGroup Group{.Name = AiMesh->mName.C_Str()};
        SubMesh   Sub{
            .VertexCount  = AiMesh->mNumVertices,
            .MaterialSlot = AiMesh->mMaterialIndex,
            .HasUV0       = AiMesh->mTextureCoords[0] != nullptr,
            .HasTangents  = AiMesh->mTangents != nullptr && AiMesh->mBitangents != nullptr,
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

[[nodiscard]] auto ResourceMesh::GetImportedMaterials() const -> const std::vector<ImportedPbrMaterial>& {
    return m_ImportedMaterials;
}

[[nodiscard]] auto ResourceMesh::GetImportedMaterial(Uint32 MaterialSlot) const -> const PbrMetallicRoughnessMaterial* {
    if (MaterialSlot >= m_ImportedMaterials.size())
        return nullptr;
    return &m_ImportedMaterials[MaterialSlot].Material;
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

[[nodiscard]] auto SubmitMeshBufferRequests(const ResourceMesh& Mesh) -> std::vector<MeshBufferRequestHandles> {
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
                .UV       = RequestMeshVertexBuffer(
                    {.Data = std::as_bytes(std::span{Sub.UVs}), .VertexCount = Sub.UVs.size(), .Stride = sizeof(hlslpp::interop::float2)}),
                .Index = RequestMeshIndexBuffer({.Data = std::as_bytes(std::span{Sub.Indices}), .IndexCount = Sub.Indices.size()}),
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

[[nodiscard]] auto SubmitMeshRequest(ResourceContext& Context, StringView MeshPath) -> ResourceHandle<ResourceMesh> {
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
            Pending->BufferHandles = SubmitMeshBufferRequests(*ImportedMesh);
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
