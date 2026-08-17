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
import :Geometry;
import TaskGraph;
export import std;

namespace SoulEngine {

[[nodiscard]] auto NormalizeMeshResourcePath(StringView InPath) -> String {
    return Path(String(InPath)).lexically_normal().string();
}

/// @brief Request mesh resource using GeometryManager.
///
/// This function imports the mesh via GeometryManager and creates a ResourceMesh
/// that only stores the mesh name. Actual geometry data is managed by GeometryManager.
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

            // Import mesh using Assimp
            Assimp::Importer Importer;
            constexpr Uint32 Flags = aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_CalcTangentSpace |
                                     aiProcess_GenSmoothNormals | aiProcess_JoinIdenticalVertices |
                                     aiProcess_ImproveCacheLocality | aiProcess_OptimizeMeshes;

            const auto* Scene = Importer.ReadFile(String(Key).c_str(), Flags);
            if (!Scene || !Scene->mRootNode || Scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) {
                PublishResourceFailed<ResourceMesh>(
                    Context, Generation, Key,
                    ErrorMessage(Format("Assimp import failed '{}': {}", Key, Importer.GetErrorString())));
                return;
            }

            if (Context.IsShutdownRequested())
                return;

            const Path MeshDirectory = Path(String(Key)).parent_path();

            // Register materials from the scene
            MaterialManager::Get().RegisterMaterialsFromScene(Scene, MeshDirectory);

            // Register geometry with GeometryManager (this also requests GPU buffers)
            auto& GeometryMgr = GeometryManager::Get();
            GeometryMgr.RegisterScene(Scene, MeshDirectory, Key);

            // Create ResourceMesh with just the mesh name
            auto ImportedMesh       = std::make_unique<ResourceMesh>();
            ImportedMesh->SetMeshName(Path(String(Key)).stem().string());

            PublishResourceReady<ResourceMesh>(
                Context, Generation, Key, {.Object = std::move(ImportedMesh)});
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
