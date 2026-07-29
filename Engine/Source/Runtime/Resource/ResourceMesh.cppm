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

[[nodiscard]] auto GetExternalTexturePath(const aiMaterial& Material,
                                          aiTextureType Type,
                                          ImportedTextureSemantic Semantic,
                                          const Path& MeshDirectory)
    -> String {
    const auto TextureCount = Material.GetTextureCount(Type);
    if (TextureCount == 0)
        return {};
    if (TextureCount != 1) {
        LogWarning("Assimp material {} texture disabled: layered texture stacks are not supported", TextureSemanticName(Semantic));
        return {};
    }

    aiString TexturePath = {};
    aiTextureMapping Mapping = aiTextureMapping_UV;
    Uint32 UVIndex = 0;
    if (Material.GetTexture(Type, 0, &TexturePath, &Mapping, &UVIndex) != AI_SUCCESS) {
        LogWarning("Assimp material {} texture disabled: failed to read texture binding", TextureSemanticName(Semantic));
        return {};
    }
    if (Mapping != aiTextureMapping_UV || UVIndex != 0) {
        LogWarning("Assimp material {} texture disabled: only UV0 mapping is supported", TextureSemanticName(Semantic));
        return {};
    }

    const Path SourcePath = TexturePath.C_Str();
    if (TexturePath.length == 0 || TexturePath.C_Str()[0] == '*') {
        LogWarning("Assimp material {} texture disabled: embedded textures are not supported", TextureSemanticName(Semantic));
        return {};
    }
    if (SourcePath.is_absolute()) {
        LogWarning("Assimp material {} texture disabled: absolute paths are not supported", TextureSemanticName(Semantic));
        return {};
    }

    const auto Normalized = (MeshDirectory / SourcePath).lexically_normal();
    if (Normalized.empty()) {
        LogWarning("Assimp material {} texture disabled: path could not be normalized", TextureSemanticName(Semantic));
        return {};
    }
    return Normalized.string();
}

[[nodiscard]] auto ReadColor(const aiMaterial& Material,
                             const char* Key,
                             Uint32 Type,
                             Uint32 Index,
                             hlslpp::float3 Fallback)
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
    Result.Material.Roughness = std::clamp(ReadFloat(*AiMaterial, AI_MATKEY_ROUGHNESS_FACTOR, Result.Material.Roughness), 0.0f, 1.0f);

    Result.Material.BaseColorTexture = GetExternalTexturePath(
        *AiMaterial, aiTextureType_BASE_COLOR, ImportedTextureSemantic::BaseColor, MeshDirectory);
    if (Result.Material.BaseColorTexture.empty())
        Result.Material.BaseColorTexture = GetExternalTexturePath(
            *AiMaterial, aiTextureType_DIFFUSE, ImportedTextureSemantic::BaseColor, MeshDirectory);
    Result.Material.NormalTexture = GetExternalTexturePath(
        *AiMaterial, aiTextureType_NORMAL_CAMERA, ImportedTextureSemantic::Normal, MeshDirectory);
    if (Result.Material.NormalTexture.empty())
        Result.Material.NormalTexture = GetExternalTexturePath(
            *AiMaterial, aiTextureType_NORMALS, ImportedTextureSemantic::Normal, MeshDirectory);
    Result.Material.MetallicTexture = GetExternalTexturePath(
        *AiMaterial, aiTextureType_METALNESS, ImportedTextureSemantic::Metallic, MeshDirectory);
    Result.Material.RoughnessTexture = GetExternalTexturePath(
        *AiMaterial, aiTextureType_DIFFUSE_ROUGHNESS, ImportedTextureSemantic::Roughness, MeshDirectory);
    Result.Material.OcclusionTexture = GetExternalTexturePath(
        *AiMaterial, aiTextureType_AMBIENT_OCCLUSION, ImportedTextureSemantic::Occlusion, MeshDirectory);
    Result.Material.EmissiveTexture = GetExternalTexturePath(
        *AiMaterial, aiTextureType_EMISSION_COLOR, ImportedTextureSemantic::Emissive, MeshDirectory);
    if (Result.Material.EmissiveTexture.empty())
        Result.Material.EmissiveTexture = GetExternalTexturePath(
            *AiMaterial, aiTextureType_EMISSIVE, ImportedTextureSemantic::Emissive, MeshDirectory);

    if (!Result.Material.MetallicTexture.empty() && Result.Material.MetallicTexture == Result.Material.RoughnessTexture) {
        Result.Material.MetallicRoughnessTexture = Result.Material.MetallicTexture;
        Result.Material.MetallicTexture = {};
        Result.Material.RoughnessTexture = {};
    }
    return Result;
}

} // namespace

[[nodiscard]] auto ParseAssimpMeshes(StringView MeshPath, ResourceMesh& Out) -> std::expected<void, ErrorMessage> {
    Assimp::Importer Importer;

    constexpr Uint32 Flags = aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_CalcTangentSpace |
                             aiProcess_GenSmoothNormals | aiProcess_JoinIdenticalVertices | aiProcess_ImproveCacheLocality |
                             aiProcess_OptimizeMeshes;

    const auto* Scene = Importer.ReadFile(String(MeshPath).c_str(), Flags);
    if (!Scene || !Scene->mRootNode || Scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)
        return std::unexpected(
            ErrorMessage(Format("Assimp import failed '{}': {}", MeshPath, Importer.GetErrorString())));

    Out.m_Name = Path(String(MeshPath)).stem().string();
    const Path MeshDirectory = Path(String(MeshPath)).parent_path();
    Out.m_ImportedMaterials.reserve(Scene->mNumMaterials);
    for (Uint32 MaterialIndex = 0; MaterialIndex < Scene->mNumMaterials; ++MaterialIndex)
        Out.m_ImportedMaterials.emplace_back(ImportMaterial(Scene->mMaterials[MaterialIndex], MeshDirectory, MaterialIndex));
    if (!Scene->HasMeshes())
        return {};

    Out.m_MeshGroups.reserve(Scene->mNumMeshes);
    for (Uint32 MeshIndex = 0; MeshIndex < Scene->mNumMeshes; ++MeshIndex) {
        const auto* AiMesh = Scene->mMeshes[MeshIndex];
        if (!AiMesh || !AiMesh->mNumVertices || !AiMesh->HasFaces())
            continue;

        MeshGroup Group{.Name = AiMesh->mName.C_Str()};
        SubMesh Sub{
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

            const auto Tangent = AiMesh->mTangents ? AiMesh->mTangents[VertexIndex] : aiVector3D{1.0f, 0.0f, 0.0f};
            float Handedness = 1.0f;
            if (Sub.HasTangents) {
                const auto Bitangent = AiMesh->mBitangents[VertexIndex];
                const auto Cross = Normal ^ Tangent;
                Handedness = (Cross * Bitangent) < 0.0f ? -1.0f : 1.0f;
            }
            Sub.Tangents.push_back(hlslpp::interop::float4{hlslpp::float4{Tangent.x, Tangent.y, Tangent.z, Handedness}});

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

auto UploadMeshBuffers(ResourceContext& Context, const String& MeshKey, ResourceMesh& InMesh) -> void {
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

[[nodiscard]] auto SubmitMeshRequest(ResourceContext& Context, StringView MeshPath) -> ResourceHandle<ResourceMesh> {
    const auto Key = NormalizeMeshResourcePath(MeshPath);
    auto       Work = BeginResourceWork<ResourceMesh>(Context, Key);
    if (!Work.ShouldStartWork)
        return Work.Handle;

    auto* ContextPtr = &Context;
    auto EnqueueResult = TaskGraph::Get().EnqueueBackground(
        [ContextPtr, Generation = Work.Handle.GetGeneration(), Key] {
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

        UploadMeshBuffers(Context, Key, *ImportedMesh);
        PublishResourceReady<ResourceMesh>(Context, Generation, Key, {.Object = std::move(ImportedMesh)});
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
