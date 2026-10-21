module;

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <entt/entt.hpp>
#include <hlsl++.h>

module Scene.Mesh;

namespace SoulEngine {

auto GeometryLoader::operator()(StringView MeshPath, Uint32 MeshIndex, const aiMesh* AiMesh) const -> result_type {
    if (!AiMesh || AiMesh->mNumVertices == 0 || !AiMesh->HasFaces()) {
        LogWarning("GeometryLoader: invalid mesh {}[{}]", MeshPath, MeshIndex);
        return nullptr;
    }

    const bool HasUV0      = AiMesh->mTextureCoords[0] != nullptr;
    // aiProcess_CalcTangentSpace is requested by MeshLoader below. With generated normals and UV0, the
    // imported tangent is the authoritative tangent input; Bitangent is intentionally derived by consumers.
    const bool HasTangents = HasUV0;

    std::vector<hlslpp::interop::float3> Positions;
    std::vector<hlslpp::interop::float3> Normals;
    std::vector<hlslpp::interop::float4> Tangents;
    std::vector<hlslpp::interop::float2> UVs;
    std::vector<Uint32>                  Indices;
    Positions.reserve(AiMesh->mNumVertices);
    Normals.reserve(AiMesh->mNumVertices);
    if (HasTangents)
        Tangents.reserve(AiMesh->mNumVertices);
    if (HasUV0)
        UVs.reserve(AiMesh->mNumVertices);

    for (Uint32 VertexIndex = 0; VertexIndex < AiMesh->mNumVertices; ++VertexIndex) {
        const auto& Position = AiMesh->mVertices[VertexIndex];
        Positions.emplace_back(hlslpp::interop::float3{hlslpp::float3{Position.x, Position.y, Position.z}});

        const auto Normal = AiMesh->mNormals ? AiMesh->mNormals[VertexIndex] : aiVector3D{0, 1, 0};
        Normals.emplace_back(hlslpp::interop::float3{hlslpp::float3{Normal.x, Normal.y, Normal.z}});

        if (HasTangents) {
            const auto& Tangent = AiMesh->mTangents[VertexIndex];
            Tangents.emplace_back(hlslpp::interop::float4{hlslpp::float4{Tangent.x, Tangent.y, Tangent.z, 1.0f}});
        }

        if (HasUV0) {
            const auto& UV = AiMesh->mTextureCoords[0][VertexIndex];
            UVs.emplace_back(hlslpp::interop::float2{hlslpp::float2{UV.x, UV.y}});
        }
    }

    Indices.reserve(AiMesh->mNumFaces * 3);
    for (Uint32 FaceIndex = 0; FaceIndex < AiMesh->mNumFaces; ++FaceIndex) {
        const auto& Face = AiMesh->mFaces[FaceIndex];
        if (Face.mNumIndices != 3) {
            LogWarning("GeometryLoader: non-triangle face in {}[{}]", MeshPath, MeshIndex);
            return nullptr;
        }
        Indices.insert(Indices.end(), Face.mIndices, Face.mIndices + 3);
    }

    const auto ComputeBoundingSphere =
        [](const std::vector<hlslpp::interop::float3>& Vertices) -> hlslpp::interop::float4 {
        if (Vertices.empty())
            return hlslpp::interop::float4{hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f}};

        hlslpp::float3 Min = hlslpp::float3{Vertices.front().x, Vertices.front().y, Vertices.front().z};
        hlslpp::float3 Max = Min;
        for (const auto& Vertex : Vertices) {
            Min.x = std::min(static_cast<float>(Min.x), static_cast<float>(Vertex.x));
            Min.y = std::min(static_cast<float>(Min.y), static_cast<float>(Vertex.y));
            Min.z = std::min(static_cast<float>(Min.z), static_cast<float>(Vertex.z));
            Max.x = std::max(static_cast<float>(Max.x), static_cast<float>(Vertex.x));
            Max.y = std::max(static_cast<float>(Max.y), static_cast<float>(Vertex.y));
            Max.z = std::max(static_cast<float>(Max.z), static_cast<float>(Vertex.z));
        }

        const auto Center        = (Min + Max) * 0.5f;
        float      RadiusSquared = 0.0f;
        for (const auto& Vertex : Vertices) {
            const auto Offset = hlslpp::float3{Vertex.x, Vertex.y, Vertex.z} - Center;
            RadiusSquared     = std::max(RadiusSquared, static_cast<float>(hlslpp::dot(Offset, Offset)));
        }

        return hlslpp::interop::float4{hlslpp::float4{Center.x, Center.y, Center.z, std::sqrt(RadiusSquared)}};
    };

    // lazy upload: the loader fills CPU data only. RHI buffer creation is deferred to the renderer-owned
    // GeometryUploader, which runs on the renderer thread on first use.
    LogDebug("GeometryLoader: \"{}[{}]\" successfully loaded, with {} positions, {} normals, {} tangents, {} uv "
             "coordinates, {} indices",
             MeshPath,
             AiMesh->mName.C_Str(),
             Positions.size(),
             Normals.size(),
             Tangents.size(),
             UVs.size(),
             Indices.size());

    return std::make_shared<GeometryRecord>(GeometryRecord{
        .Name                = AiMesh->mName.C_Str(),
        .IndexCount          = static_cast<Uint32>(Indices.size()),
        .HasUV0              = HasUV0,
        .HasTangents         = HasTangents,
        .LocalBoundingSphere = ComputeBoundingSphere(Positions),
        .Positions           = std::move(Positions),
        .Normals             = std::move(Normals),
        .Tangents            = std::move(Tangents),
        .UVs                 = std::move(UVs),
        .Indices             = std::move(Indices),
    });
}

auto MeshLoader::operator()(const Path& MeshPath) -> result_type {
    const auto       MeshPathString = MeshPath.string();
    Assimp::Importer Importer;
    // CalcTangentSpace generates the tangent from UV0 and the normals generated above. Bitangent is
    // deliberately not retained; shaders reconstruct it from the normal and tangent when needed.
    constexpr Uint32 Flags = aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_CalcTangentSpace |
                             aiProcess_GenSmoothNormals | aiProcess_JoinIdenticalVertices |
                             aiProcess_ImproveCacheLocality | aiProcess_OptimizeMeshes |
                             aiProcess_ValidateDataStructure;
    const auto*      Scene = Importer.ReadFile(MeshPathString.c_str(), Flags);
    if (!Scene || !Scene->mRootNode || Scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) {
        LogWarning("MeshLoader: Assimp import failed '{}': {}", MeshPathString, Importer.GetErrorString());
        return nullptr;
    }

    std::vector<MaterialHandle> Materials = {};
    Materials.reserve(Scene->mNumMaterials);
    for (Uint32 MaterialIndex = 0; MaterialIndex < Scene->mNumMaterials; ++MaterialIndex) {
        const auto* AiMaterial = Scene->mMaterials[MaterialIndex];
        const auto  ResourceId = MakeMaterialCacheKey(MeshPath, MaterialIndex);
        auto [It, Loaded]      = m_MaterialAssimpCache.load(ResourceId, AiMaterial, MeshPath);
        if (!It->second) {
            LogWarning("MeshLoader: material load failed '{}#{}'", MeshPathString, MaterialIndex);
            return nullptr;
        }
        Materials.emplace_back(It->second);
    }

    auto Result = std::make_shared<MeshRecord>(MeshRecord{
        .Asset     = MeshPath,
        .Materials = std::move(Materials),
        .SubMeshes = {},
        .Nodes     = {},
    });
    Result->SubMeshes.reserve(Scene->mNumMeshes);
    for (Uint32 MeshIndex = 0; MeshIndex < Scene->mNumMeshes; ++MeshIndex) {
        const auto KeyText       = Format("{}#{}", MeshPathString, MeshIndex);
        const auto ResourceId    = entt::hashed_string{KeyText.data(), KeyText.size()};
        const auto MaterialIndex = Scene->mMeshes[MeshIndex]->mMaterialIndex;
        const auto Material =
            MaterialIndex < Result->Materials.size() ? Result->Materials[MaterialIndex] : MaterialHandle{};
        auto [It, Loaded] = m_GeometryCache.load(ResourceId, MeshPathString, MeshIndex, Scene->mMeshes[MeshIndex]);
        if (!It->second) {
            LogWarning("MeshLoader: geometry load failed '{}[{}]'", MeshPathString, MeshIndex);
            return nullptr;
        }
        Result->SubMeshes.emplace_back(SubMesh{
            .Name     = Scene->mMeshes[MeshIndex]->mName.C_Str(),
            .Geometry = It->second,
            .Material = Material,
        });
    }

    const auto ConvertTransform = [](const aiMatrix4x4& Transform) -> hlslpp::float4x4 {
        // Assimp transforms column vectors, while SoulEngine uses row vectors. Copying Assimp's columns
        // into hlsl++ rows preserves the transform.
        auto ResultTransform = hlslpp::float4x4::identity();
        ResultTransform[0]   = hlslpp::float4(Transform.a1, Transform.b1, Transform.c1, Transform.d1);
        ResultTransform[1]   = hlslpp::float4(Transform.a2, Transform.b2, Transform.c2, Transform.d2);
        ResultTransform[2]   = hlslpp::float4(Transform.a3, Transform.b3, Transform.c3, Transform.d3);
        ResultTransform[3]   = hlslpp::float4(Transform.a4, Transform.b4, Transform.c4, Transform.d4);
        return ResultTransform;
    };
    const auto ImportNode = [&Result, &MeshPathString, &ConvertTransform](auto&&        ImportNode,
                                                                          const aiNode* AiNode) -> bool {
        if (!AiNode || (AiNode->mNumMeshes != 0 && !AiNode->mMeshes) ||
            (AiNode->mNumChildren != 0 && !AiNode->mChildren)) {
            LogWarning("MeshLoader: invalid node in '{}'", MeshPathString);
            return false;
        }

        const auto NodeIndex = static_cast<Uint32>(Result->Nodes.size());
        Result->Nodes.emplace_back(MeshAssetNode{
            .Name           = AiNode->mName.C_Str(),
            .LocalTransform = ConvertTransform(AiNode->mTransformation),
        });
        for (Uint32 MeshIndex = 0; MeshIndex < AiNode->mNumMeshes; ++MeshIndex) {
            const auto SubMeshIndex = AiNode->mMeshes[MeshIndex];
            if (SubMeshIndex >= Result->SubMeshes.size()) {
                LogWarning("MeshLoader: node '{}' references invalid mesh {} in '{}'",
                           AiNode->mName.C_Str(),
                           SubMeshIndex,
                           MeshPathString);
                return false;
            }
            Result->Nodes[NodeIndex].SubMeshIndices.emplace_back(SubMeshIndex);
        }
        for (Uint32 ChildIndex = 0; ChildIndex < AiNode->mNumChildren; ++ChildIndex) {
            const auto ChildNodeIndex = static_cast<Uint32>(Result->Nodes.size());
            if (!ImportNode(ImportNode, AiNode->mChildren[ChildIndex]))
                return false;
            Result->Nodes[NodeIndex].ChildNodeIndices.emplace_back(ChildNodeIndex);
        }
        return true;
    };
    if (!ImportNode(ImportNode, Scene->mRootNode))
        return nullptr;

    const auto DumpNode = [&Result](auto&& DumpNode, Uint32 NodeIndex, Uint32 Depth) -> void {
        if (NodeIndex >= Result->Nodes.size())
            return;

        const auto& Node        = Result->Nodes[NodeIndex];
        const auto  Indent      = String(Depth * 2, ' ');
        const auto& Translation = Node.LocalTransform[3];
        LogDebug("MeshLoader: {}node '{}' local translation ({:.3f}, {:.3f}, {:.3f})",
                 Indent,
                 Node.Name,
                 static_cast<float>(Translation.x),
                 static_cast<float>(Translation.y),
                 static_cast<float>(Translation.z));
        for (const auto SubMeshIndex : Node.SubMeshIndices)
            LogDebug("MeshLoader: {}  submesh[{}]", Indent, SubMeshIndex);
        for (const auto ChildNodeIndex : Node.ChildNodeIndices)
            DumpNode(DumpNode, ChildNodeIndex, Depth + 1);
    };
    DumpNode(DumpNode, 0, 0);

    LogInfo("MeshLoader: \"{}({} geometries, {} nodes)\" successfully loaded",
            MeshPathString,
            Result->SubMeshes.size(),
            Result->Nodes.size());

    return Result;
}

using namespace entt::literals;

struct MeshComponentMetaRegistration {
    MeshComponentMetaRegistration() {
        entt::meta_factory<MeshComponent>{"mesh"_hs}
            .data<&MeshComponent::Asset>("asset"_hs)
            .data<&MeshComponent::MaterialOverridePath>("material_override"_hs)
            .func<&EmplaceComponent<MeshComponent>>("emplace"_hs);
    }
};

MeshComponentMetaRegistration g_MeshComponentMetaRegistration = {};

auto MeshSystem::SetAssetRoot(Path AssetRoot) -> void {
    m_AssetRoot = AssetRoot.lexically_normal();
}

auto MeshSystem::OnUpdate(Float32 DeltaTime) -> void {
    const auto AppendMaterialTextures = [this](const MaterialHandle& Material) -> void {
        if (!Material)
            return;
        for (const auto& TextureSlot : Material->Textures) {
            // Texture creation is synchronous and publishes Ready at return, so the guard below only
            // filters empty refs; texel bytes land via UploadTexture's frame-start batch by submission order.
            if (!TextureSlot.Texture || TextureSlot.Texture->Texture.GetState() == RHIRefState::Unknown)
                continue;
            if (std::ranges::find(m_Textures, TextureSlot.Texture->Texture) == m_Textures.end())
                m_Textures.push_back(TextureSlot.Texture->Texture);
        }
    };
    const auto AppendMeshTextures = [&AppendMaterialTextures](const MeshRecord& Mesh) -> void {
        for (const auto& SubMeshValue : Mesh.SubMeshes)
            AppendMaterialTextures(SubMeshValue.Material);
    };

    const auto MeshView = m_Registry.view<MeshComponent>();
    for (const auto Entity : MeshView) {
        auto& Mesh = MeshView.get<MeshComponent>(Entity);
        if (Mesh.Asset.empty()) {
            Mesh.Mesh = {};
            continue;
        }

        const auto MeshPath = m_AssetRoot / Mesh.Asset;
        if (!Mesh.Mesh || Mesh.Mesh->Asset != MeshPath) {
            Mesh.Mesh                 = {};
            const auto MeshPathString = MeshPath.string();
            const auto ResourceId     = entt::hashed_string{MeshPathString.data(), MeshPathString.size()};
            auto [It, Loaded]         = m_MeshCache.load(ResourceId, MeshPath);
            if (It->second)
                Mesh.Mesh = It->second;
            if (Loaded && Mesh.Mesh)
                AppendMeshTextures(*Mesh.Mesh);
        }

        Mesh.MaterialOverride = {};
        if (!Mesh.MaterialOverridePath.empty()) {
            const auto MaterialPath       = (m_AssetRoot / Mesh.MaterialOverridePath).lexically_normal();
            const auto MaterialPathString = MaterialPath.string();
            const auto MaterialId         = entt::hashed_string{MaterialPathString.data(), MaterialPathString.size()};
            auto [MaterialIt, MaterialLoaded] = m_MaterialYamlCache.load(MaterialId, MaterialPath, m_AssetRoot);
            if (MaterialIt->second)
                Mesh.MaterialOverride = MaterialIt->second;
            else
                LogWarning("MeshSystem: material override load failed '{}'", MaterialPathString);
        }
        AppendMaterialTextures(Mesh.MaterialOverride);
    }
}

auto MeshSystem::CollectInstances() const -> std::vector<InstanceRecord> {
    std::vector<InstanceRecord> Result;
    const auto                  MeshView = m_Registry.view<MeshComponent, TransformComponent>();
    for (const auto Entity : MeshView) {
        const auto& Mesh = MeshView.get<MeshComponent>(Entity);
        if (Mesh.Asset.empty() || !Mesh.Mesh)
            continue;
        const auto& Transform = MeshView.get<TransformComponent>(Entity);
        const auto  VisitNode = [&Mesh, Entity, &Result](auto&&                  VisitNode,
                                                         Uint32                  NodeIndex,
                                                         const hlslpp::float4x4& ParentWorldTransform) -> void {
            if (NodeIndex >= Mesh.Mesh->Nodes.size())
                return;

            const auto& Node           = Mesh.Mesh->Nodes[NodeIndex];
            const auto  WorldTransform = hlslpp::mul(Node.LocalTransform, ParentWorldTransform);
            for (const Uint32 SubMeshIndex : Node.SubMeshIndices) {
                if (SubMeshIndex >= Mesh.Mesh->SubMeshes.size())
                    continue;
                const auto& MeshSubMesh = Mesh.Mesh->SubMeshes[SubMeshIndex];
                if (!MeshSubMesh.Geometry)
                    continue;
                Result.emplace_back(InstanceRecord{
                    .EntityId       = entt::to_integral(Entity),
                    .Geometry       = MeshSubMesh.Geometry,
                    .Material       = Mesh.MaterialOverride ? Mesh.MaterialOverride : MeshSubMesh.Material,
                    .WorldTransform = WorldTransform,
                });
            }
            for (const Uint32 ChildNodeIndex : Node.ChildNodeIndices)
                VisitNode(VisitNode, ChildNodeIndex, WorldTransform);
        };
        VisitNode(VisitNode, 0, Transform.WorldTransform);
    }
    return Result;
}

auto MeshSystem::CollectMaterialRecords() const -> std::vector<ConstMaterialHandle> {
    std::vector<ConstMaterialHandle> Result;
    for (const auto& Entry : m_MaterialYamlCache) {
        const auto& Material = Entry.second;
        if (Material)
            Result.emplace_back(Material);
    }

    for (const auto& Entry : m_MeshCache) {
        const auto& Mesh = Entry.second;
        if (!Mesh)
            continue;

        for (const auto& Material : Mesh->Materials) {
            if (Material)
                Result.emplace_back(Material);
        }
    }
    return Result;
}

auto MeshSystem::GetTextures() const -> const std::vector<RHIRef<RHITexture>>& {
    return m_Textures;
}

} // namespace SoulEngine
