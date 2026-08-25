module;

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <cstddef>
#include <entt/entt.hpp>
#include <hlsl++.h>

export module Scene:Mesh;

export import Core;
export import RHI;
export import Resource;

export namespace SoulEngine {

/// @brief GPU-ready geometry record for one imported submesh.
///
/// The record owns imported CPU metadata and ref-backed GPU buffers. MeshSystem
/// owns the record through the scene-local mesh resource cache.
///
/// Tangents are stored as xyz plus a fixed-handedness w component. Bitangents
/// are intentionally not imported or stored; consumers derive them from the
/// normal and tangent with cross(normal, tangent).
struct GeometryRecord {
    String Name = {};
    // TODO: Delete this after refractoring RayTracingRenderer
    String CacheKey = {};

    RHIRef<RHIVertexBuffer> PositionBuffer = nullptr;
    RHIRef<RHIVertexBuffer> NormalBuffer   = nullptr;
    RHIRef<RHIVertexBuffer> TangentBuffer  = nullptr;
    RHIRef<RHIVertexBuffer> TexCoordBuffer = nullptr;
    RHIRef<RHIIndexBuffer>  IndexBuffer    = nullptr;

    /// @brief Shader ABI for the raster geometry address table.
    struct alignas(8) GpuData {
        Uint64 PositionAddress = 0;
        Uint64 NormalAddress   = 0;
        Uint64 TangentAddress  = 0;
        Uint64 TexCoordAddress = 0;
        Uint64 IndexAddress    = 0;
        hlslpp::interop::float4 BoundingSphere = hlslpp::interop::float4{
            hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f}};
    };
    static_assert(sizeof(GpuData) == 56);
    static_assert(alignof(GpuData) == 8);
    static_assert(offsetof(GpuData, PositionAddress) == 0);
    static_assert(offsetof(GpuData, NormalAddress) == 8);
    static_assert(offsetof(GpuData, TangentAddress) == 16);
    static_assert(offsetof(GpuData, TexCoordAddress) == 24);
    static_assert(offsetof(GpuData, IndexAddress) == 32);
    static_assert(offsetof(GpuData, BoundingSphere) == 40);

    [[nodiscard]] auto BuildGpuData() const -> GpuData {
        return GpuData{
            .PositionAddress = PositionBuffer ? PositionBuffer->GetDeviceAddress() : 0,
            .NormalAddress   = NormalBuffer ? NormalBuffer->GetDeviceAddress() : 0,
            .TangentAddress  = TangentBuffer ? TangentBuffer->GetDeviceAddress() : 0,
            .TexCoordAddress = TexCoordBuffer ? TexCoordBuffer->GetDeviceAddress() : 0,
            .IndexAddress    = IndexBuffer ? IndexBuffer->GetDeviceAddress() : 0,
            .BoundingSphere  = LocalBoundingSphere,
        };
    }

    Uint32 IndexCount  = 0;
    Uint32 MaterialId  = 0;
    bool   HasUV0      = false;
    bool   HasTangents = false;
    hlslpp::interop::float4 LocalBoundingSphere = hlslpp::interop::float4{
        hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f}};

    std::vector<hlslpp::interop::float3> Positions;
    std::vector<hlslpp::interop::float3> Normals;
    std::vector<hlslpp::interop::float4> Tangents;
    std::vector<hlslpp::interop::float2> UVs;
    std::vector<Uint32>                  Indices;
};

using GeometryHandle = entt::resource<GeometryRecord>;

/// @brief Cached imported mesh asset.
struct MeshRecord {
    Path                        Asset      = {};
    std::vector<GeometryHandle> Geometries = {};
};

using MeshHandle = entt::resource<MeshRecord>;

/// @brief Scene-authored mesh asset reference and runtime mesh handle.
struct MeshComponent {
    Path       Asset    = {}; ///< YAML-normalized path relative to the Scene Assets directory.
    String     Material = {};
    MeshHandle Mesh     = {};
};

/// @brief Immutable render-facing Geometry instance.
struct InstanceRecord {
    Uint32         EntityId       = 0;
    GeometryHandle Geometry       = {};
    hlslpp::float4x4 WorldTransform = hlslpp::float4x4::identity();
    Uint32         MaterialID     = 0;

    /// @brief GPU instance ABI populated after the renderer assigns GeometryID.
    struct alignas(16) GpuData {
        alignas(16) hlslpp::float4x4 WorldTransform = hlslpp::float4x4::identity();
        Uint32 MaterialID = 0;
        Uint32 EntityID   = 0;
        Uint32 GeometryID = 0;
    };
    static_assert(sizeof(GpuData) == 80);
    static_assert(offsetof(GpuData, WorldTransform) == 0);
    static_assert(offsetof(GpuData, MaterialID) == 64);
    static_assert(offsetof(GpuData, EntityID) == 68);
    static_assert(offsetof(GpuData, GeometryID) == 72);

    [[nodiscard]] auto BuildGpuData(Uint32 GeometryID) const -> GpuData {
        return GpuData{
            .WorldTransform = WorldTransform,
            .MaterialID      = MaterialID,
            .EntityID        = EntityId,
            .GeometryID      = GeometryID,
        };
    }
};

struct GeometryLoader {
    using result_type = std::shared_ptr<GeometryRecord>;

    auto operator()(StringView MeshPath, Uint32 MeshIndex, Uint32 MaterialId, const aiMesh* AiMesh) const
        -> result_type {
        if (!AiMesh || AiMesh->mNumVertices == 0 || !AiMesh->HasFaces()) {
            LogWarning("GeometryLoader: invalid mesh {}[{}]", MeshPath, MeshIndex);
            return nullptr;
        }

        const bool HasUV0      = AiMesh->mTextureCoords[0] != nullptr;
        // aiProcess_CalcTangentSpace is requested by MeshLoader below. With
        // generated normals and UV0, the imported tangent is the authoritative
        // tangent input; Bitangent is intentionally derived by consumers.
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

        const auto ComputeBoundingSphere = [](const std::vector<hlslpp::interop::float3>& Vertices)
            -> hlslpp::interop::float4 {
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

            const auto Center = (Min + Max) * 0.5f;
            float      RadiusSquared = 0.0f;
            for (const auto& Vertex : Vertices) {
                const auto Offset = hlslpp::float3{Vertex.x, Vertex.y, Vertex.z} - Center;
                RadiusSquared = std::max(RadiusSquared, static_cast<float>(hlslpp::dot(Offset, Offset)));
            }

            return hlslpp::interop::float4{hlslpp::float4{
                Center.x, Center.y, Center.z, std::sqrt(RadiusSquared)}};
        };

        auto&      Device = RHIRenderDevice::Get();
        const auto Key    = Format("{}#{}", MeshPath, MeshIndex);

        // Vertex attributes share the same RHI creation pattern; keep the
        // attribute-specific data layout at each call site while centralizing
        // resource naming and failure logging.
        const auto CreateVertexBuffer = [&Device,
                                         &Key](StringView                 AttributeName,
                                               std::span<const std::byte> Data,
                                               Uint64                     VertexCount,
                                               Uint32 Stride) -> std::expected<RHIRef<RHIVertexBuffer>, ErrorMessage> {
            auto Result = Device.CreateVertexBuffer(Format("{}/{}", Key, AttributeName),
                                                    {
                                                        .Data        = Data,
                                                        .VertexCount = VertexCount,
                                                        .Stride      = Stride,
                                                    });
            if (!Result) {
                LogError("GeometryLoader: {} buffer creation failed: {}", AttributeName, Result.error().ToString());
                return std::unexpected(Result.error());
            }
            return std::move(*Result);
        };

        const auto CreateIndexBuffer =
            [&Device, &Key](std::span<const std::byte> Data,
                            Uint64 IndexCount) -> std::expected<RHIRef<RHIIndexBuffer>, ErrorMessage> {
            auto Result = Device.CreateIndexBuffer(Format("{}/Index", Key),
                                                   {
                                                       .Data       = Data,
                                                       .IndexCount = IndexCount,
                                                   });
            if (!Result) {
                LogError("GeometryLoader: index buffer creation failed: {}", Result.error().ToString());
                return std::unexpected(Result.error());
            }
            return std::move(*Result);
        };

        auto PositionBuffer =
            CreateVertexBuffer("Position", std::as_bytes(std::span{Positions}), Positions.size(), sizeof(Positions[0]));
        if (!PositionBuffer)
            return nullptr;

        auto NormalBuffer =
            CreateVertexBuffer("Normal", std::as_bytes(std::span{Normals}), Normals.size(), sizeof(Normals[0]));
        if (!NormalBuffer)
            return nullptr;

        RHIRef<RHIVertexBuffer> TangentBuffer = nullptr;
        if (HasTangents && !Tangents.empty()) {
            auto Result =
                CreateVertexBuffer("Tangent", std::as_bytes(std::span{Tangents}), Tangents.size(), sizeof(Tangents[0]));
            if (!Result)
                return nullptr;
            TangentBuffer = std::move(*Result);
        }

        RHIRef<RHIVertexBuffer> TexCoordBuffer = nullptr;
        if (HasUV0 && !UVs.empty()) {
            auto Result = CreateVertexBuffer("TexCoord", std::as_bytes(std::span{UVs}), UVs.size(), sizeof(UVs[0]));
            if (!Result)
                return nullptr;
            TexCoordBuffer = std::move(*Result);
        }

        auto IndexBuffer = CreateIndexBuffer(std::as_bytes(std::span{Indices}), Indices.size());
        if (!IndexBuffer)
            return nullptr;

        return std::make_shared<GeometryRecord>(GeometryRecord{
            .Name           = AiMesh->mName.C_Str(),
            .CacheKey       = Key,
            .PositionBuffer = std::move(*PositionBuffer),
            .NormalBuffer   = std::move(*NormalBuffer),
            .TangentBuffer  = std::move(TangentBuffer),
            .TexCoordBuffer = std::move(TexCoordBuffer),
            .IndexBuffer    = std::move(*IndexBuffer),
            .IndexCount     = static_cast<Uint32>(Indices.size()),
            .MaterialId     = MaterialId,
            .HasUV0         = HasUV0,
            .HasTangents    = HasTangents,
            .LocalBoundingSphere = ComputeBoundingSphere(Positions),
            .Positions      = std::move(Positions),
            .Normals        = std::move(Normals),
            .Tangents       = std::move(Tangents),
            .UVs            = std::move(UVs),
            .Indices        = std::move(Indices),
        });
    }
};

using GeometryCache = entt::resource_cache<GeometryRecord, GeometryLoader>;

struct MeshLoader {
    using result_type = std::shared_ptr<MeshRecord>;

    auto operator()(const Path& MeshPath, GeometryCache& GeometryResources) const -> result_type {
        const auto       MeshPathString = MeshPath.string();
        Assimp::Importer Importer;
        // CalcTangentSpace generates the tangent from UV0 and the normals
        // generated above. Bitangent is deliberately not retained; shaders
        // reconstruct it from the normal and tangent when needed.
        constexpr Uint32 Flags = aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_CalcTangentSpace |
                                 aiProcess_GenSmoothNormals | aiProcess_JoinIdenticalVertices |
                                 aiProcess_ImproveCacheLocality | aiProcess_OptimizeMeshes;
        const auto*      Scene = Importer.ReadFile(MeshPathString.c_str(), Flags);
        if (!Scene || !Scene->mRootNode || Scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) {
            LogWarning("MeshLoader: Assimp import failed '{}': {}", MeshPathString, Importer.GetErrorString());
            return nullptr;
        }

        // Assimp material indices are scene-local; GeometryRecord stores the
        // corresponding global MaterialManager IDs used by renderers.
        const auto MaterialIds = MaterialManager::Get().RegisterMaterialsFromScene(Scene, MeshPath.parent_path());
        auto       Result      = std::make_shared<MeshRecord>(MeshRecord{
            .Asset      = MeshPath,
            .Geometries = {},
        });
        Result->Geometries.reserve(Scene->mNumMeshes);
        for (Uint32 MeshIndex = 0; MeshIndex < Scene->mNumMeshes; ++MeshIndex) {
            const auto KeyText    = Format("{}#{}", MeshPathString, MeshIndex);
            const auto ResourceId = entt::hashed_string{KeyText.data(), KeyText.size()};
            const auto MaterialId = Scene->mMeshes[MeshIndex]->mMaterialIndex < MaterialIds.size()
                                        ? MaterialIds[Scene->mMeshes[MeshIndex]->mMaterialIndex]
                                        : 0;
            auto [It, Loaded] =
                GeometryResources.load(ResourceId, MeshPathString, MeshIndex, MaterialId, Scene->mMeshes[MeshIndex]);
            if (!It->second) {
                LogWarning("MeshLoader: geometry load failed '{}[{}]'", MeshPathString, MeshIndex);
                return nullptr;
            }
            Result->Geometries.push_back(It->second);
        }
        return Result;
    }
};

using MeshCache = entt::resource_cache<MeshRecord, MeshLoader>;

/// @brief System for loading mesh assets and collecting render records.
class MeshSystem : public ISystem {
  public:
    explicit MeshSystem(entt::registry& Registry) : ISystem(Registry) {}
    ~MeshSystem() override = default;

    auto SetAssetRoot(Path AssetRoot) -> void {
        m_AssetRoot = AssetRoot.lexically_normal();
    }

    auto OnUpdate(Float32) -> void override {
        const auto MeshView = m_Registry.view<MeshComponent>();
        for (const auto Entity : MeshView) {
            auto& Mesh = MeshView.get<MeshComponent>(Entity);
            if (Mesh.Asset.empty()) {
                Mesh.Mesh = {};
                continue;
            }

            const auto MeshPath = m_AssetRoot / Mesh.Asset;
            if (Mesh.Mesh && Mesh.Mesh->Asset == MeshPath)
                continue;

            Mesh.Mesh                 = {};
            const auto MeshPathString = MeshPath.string();
            const auto ResourceId     = entt::hashed_string{MeshPathString.data(), MeshPathString.size()};
            auto [It, Loaded]         = m_MeshCache.load(ResourceId, MeshPath, m_GeometryCache);
            if (It->second)
                Mesh.Mesh = It->second;
        }
    }

    [[nodiscard]] auto CollectInstances() const -> std::vector<InstanceRecord> {
        std::vector<InstanceRecord> Result;
        const auto                      MeshView = m_Registry.view<MeshComponent, TransformComponent>();
        for (const auto Entity : MeshView) {
            const auto& Mesh = MeshView.get<MeshComponent>(Entity);
            if (Mesh.Asset.empty() || !Mesh.Mesh)
                continue;
            const auto& Transform          = MeshView.get<TransformComponent>(Entity);
            const auto MaterialOverrideId =
                Mesh.Material.empty() ? std::optional<Uint32>{} :
                                        MaterialManager::Get().FindMaterialIdOptional(Mesh.Material);
            for (const auto& Geometry : Mesh.Mesh->Geometries) {
                if (!Geometry)
                    continue;
                Result.emplace_back(InstanceRecord{
                    .EntityId       = entt::to_integral(Entity),
                    .Geometry       = Geometry,
                    .WorldTransform = Transform.WorldTransform,
                    .MaterialID     = MaterialOverrideId.value_or(Geometry->MaterialId),
                });
            }
        }
        return Result;
    }

  private:
    Path          m_AssetRoot     = {};
    GeometryCache m_GeometryCache = {};
    MeshCache     m_MeshCache     = {};
};

} // namespace SoulEngine

namespace SoulEngine {

namespace {

struct MeshComponentMetaRegistration {
    MeshComponentMetaRegistration() {
        entt::meta_factory<MeshComponent>{}.type("mesh").data<&MeshComponent::Material>("material");
    }
};

MeshComponentMetaRegistration g_MeshComponentMetaRegistration = {};

} // namespace

} // namespace SoulEngine
