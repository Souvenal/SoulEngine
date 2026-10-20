module;

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <entt/entt.hpp>
#include <hlsl++.h>

module Scene:Mesh.impl;

import :Mesh;

namespace SoulEngine {

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
