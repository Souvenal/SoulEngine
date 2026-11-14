module;

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <cstddef>
#include <entt/entt.hpp>
#include <hlsl++.h>

export module Scene.Mesh;

import std;
import Core;
import Material;
import RHI;

namespace SoulEngine {

/// @brief GPU-ready geometry record for one imported submesh.
///
/// The record owns imported CPU metadata and ref-backed GPU buffers. MeshSystem owns the record through the
/// scene-local mesh resource cache.
///
/// GPU residency is lazy: the loader fills CPU data only. The renderer-owned `GeometryUploader` creates
/// buffers on first use and sets `BufferUploadRequested` — buffer creation is a synchronous pure allocation
/// on the calling thread (Ready at return, device address bakable), the bytes go to UploadBuffer as a
/// frame-affined task whose copies land in the frame-start upload batch of the same frame, and the CPU
/// streams are released. The ray-tracing path additionally queues a static BLAS descriptor once the buffers
/// are Ready and sets `BlasRequested`; the BLAS joins a frame's pending-build batch (carried by RenderResult,
/// built at frame start after the upload batch). Both flags mean "the request has been issued", never
/// "completed" — actual usability is the corresponding RHIRef reaching Ready, queried via `IsReadyOnGpu`. A
/// record may enter an RT frame with a not-yet-built BLAS, safe because the frame-start AS phase runs the
/// build before every pass.
///
/// Mutation rule: `GeometryUploader` is the sole writer of GPU state (buffers, BLAS, flags). The record
/// itself carries only const queries (`IsUploadRequested` / `IsReadyOnGpu` / `BuildGpuData`); upload is never
/// a member operation. Treat the record as read-only outside `GeometryUploader`.
///
/// Tangents are stored as xyz plus a fixed-handedness w component. Bitangents are intentionally not imported
/// or stored; consumers derive them from the normal and tangent with cross(normal, tangent).
export struct GeometryRecord {
    String Name = {};

    RHIRef<RHIBuffer>                           PositionBuffer = nullptr;
    RHIRef<RHIBuffer>                           NormalBuffer   = nullptr;
    RHIRef<RHIBuffer>                           TangentBuffer  = nullptr;
    RHIRef<RHIBuffer>                           TexCoordBuffer = nullptr;
    RHIRef<RHIBuffer>                           IndexBuffer    = nullptr;
    /// @brief Geometry metadata for the mesh's vertex and index streams.
    ///
    /// Geometry metadata is mesh data, not buffer data: filled by GeometryUploader from the CPU streams,
    /// consumed by the BLAS desc and the indirect-command derivation. IndexCount already lives below with the
    /// CPU streams.
    Uint64                                      VertexCount    = 0;
    Uint32                                      PositionStride = 0;
    /// @brief Geometry-owned static BLAS; created and built on demand by the RT path.
    ///
    /// Built exactly once and never updated.
    RHIRef<RHIBottomLevelAccelerationStructure> Blas           = nullptr;

    /// @brief Set by GeometryUploader once buffer creation has been queued to the RHI thread.
    bool BufferUploadRequested = false;
    /// @brief Set by GeometryUploader once BLAS creation has been queued; the
    /// same step enters the BLAS into the pending build batch.
    bool BlasRequested         = false;

    /// @brief Whether the upload request has been issued: buffers always; with
    /// NeedBLAS, the BLAS allocation request as well. "Requested", not "completed" — see IsReadyOnGpu for
    /// actual usability.
    [[nodiscard]] auto IsUploadRequested(bool NeedBLAS) const -> bool {
        return BufferUploadRequested && (!NeedBLAS || BlasRequested);
    }

    /// @brief Whether the requested resources are usable on the GPU: every
    /// required buffer ref Ready (= allocated, device address bakable; the bytes land in the carrying frame's
    /// frame-start upload batch); with NeedBLAS, the BLAS allocation ref Ready as well. Absent tangent or
    /// texcoord streams do not block readiness unless VertexInput is requested. Does NOT track the deferred
    /// BLAS build (storage materializes at the frame-start AS phase).
    [[nodiscard]] auto IsReadyOnGpu(bool NeedBLAS, bool NeedVertexInput = false) const -> bool {
        if (!BufferUploadRequested)
            return false;
        const auto IsReady = [](const auto& Ref) { return Ref.TryGet() != nullptr; };
        if (!IsReady(PositionBuffer) || !IsReady(NormalBuffer) || !IsReady(IndexBuffer))
            return false;
        if ((HasTangents || NeedVertexInput) && !IsReady(TangentBuffer))
            return false;
        if ((HasUV0 || NeedVertexInput) && !IsReady(TexCoordBuffer))
            return false;
        if (NeedBLAS && (!BlasRequested || !IsReady(Blas)))
            return false;
        return true;
    }

    /// @brief Shader ABI for the raster geometry address table.
    struct alignas(16) GpuData {
        hlslpp::interop::float4 BoundingSphere  = hlslpp::interop::float4{hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f}};
        Uint64                  PositionAddress = 0;
        Uint64                  NormalAddress   = 0;
        Uint64                  TangentAddress  = 0;
        Uint64                  TexCoordAddress = 0;
        Uint64                  IndexAddress    = 0;
    };
    static_assert(sizeof(GpuData) == 64);
    static_assert(alignof(GpuData) == 16);
    static_assert(offsetof(GpuData, BoundingSphere) == 0);
    static_assert(offsetof(GpuData, PositionAddress) == 16);
    static_assert(offsetof(GpuData, NormalAddress) == 24);
    static_assert(offsetof(GpuData, TangentAddress) == 32);
    static_assert(offsetof(GpuData, TexCoordAddress) == 40);
    static_assert(offsetof(GpuData, IndexAddress) == 48);

    [[nodiscard]] auto BuildGpuData() const -> GpuData {
        return GpuData{
            .BoundingSphere  = LocalBoundingSphere,
            .PositionAddress = PositionBuffer ? PositionBuffer->GetDeviceAddress() : 0,
            .NormalAddress   = NormalBuffer ? NormalBuffer->GetDeviceAddress() : 0,
            .TangentAddress  = HasTangents && TangentBuffer ? TangentBuffer->GetDeviceAddress() : 0,
            .TexCoordAddress = HasUV0 && TexCoordBuffer ? TexCoordBuffer->GetDeviceAddress() : 0,
            .IndexAddress    = IndexBuffer ? IndexBuffer->GetDeviceAddress() : 0,
        };
    }

    Uint32                  IndexCount          = 0;
    bool                    HasUV0              = false;
    bool                    HasTangents         = false;
    hlslpp::interop::float4 LocalBoundingSphere = hlslpp::interop::float4{hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f}};

    // CPU streams, released by GeometryUploader once the buffer stage succeeds (the byte snapshots move into
    // the buffer payloads).
    std::vector<hlslpp::interop::float3> Positions;
    std::vector<hlslpp::interop::float3> Normals;
    std::vector<hlslpp::interop::float4> Tangents;
    std::vector<hlslpp::interop::float2> UVs;
    std::vector<Uint32>                  Indices;
};

export using GeometryHandle = entt::resource<GeometryRecord>;

/// @brief One imported submesh and its default material binding.
export struct SubMesh {
    String         Name     = {};
    GeometryHandle Geometry = {};
    MaterialHandle Material = {};
};

/// @brief One static node in an imported mesh asset hierarchy.
export struct MeshAssetNode {
    String              Name             = {};
    hlslpp::float4x4    LocalTransform   = hlslpp::float4x4::identity();
    std::vector<Uint32> SubMeshIndices   = {};
    std::vector<Uint32> ChildNodeIndices = {};
};

/// @brief Cached imported mesh asset.
export struct MeshRecord {
    Path                        Asset     = {};
    std::vector<MaterialHandle> Materials = {};
    std::vector<SubMesh>        SubMeshes = {};
    std::vector<MeshAssetNode>  Nodes     = {};
};

export using MeshHandle = entt::resource<MeshRecord>;

/// @brief Scene-authored mesh asset reference and runtime mesh handle.
export struct MeshComponent {
    Path           Asset                = {}; ///< YAML-normalized path relative to the Scene Assets directory.
    Path           MaterialOverridePath = {}; ///< YAML-normalized path relative to the Scene Assets directory.
    MaterialHandle MaterialOverride     = {};
    MeshHandle     Mesh                 = {};

    /// @brief Field descriptors of the YAML-persisted members.
    static constexpr std::array Fields = {
        Field<&MeshComponent::Asset>("asset"),
        Field<&MeshComponent::MaterialOverridePath>("material_override"),
    };
};

/// @brief Immutable render-facing geometry/material instance.
export struct InstanceRecord {
    Uint32           EntityId       = 0;
    GeometryHandle   Geometry       = {};
    MaterialHandle   Material       = {};
    hlslpp::float4x4 WorldTransform = hlslpp::float4x4::identity();

    /// @brief GPU instance ABI populated after the renderer assigns geometry/material IDs.
    struct alignas(16) GpuData {
        alignas(16) hlslpp::float4x4 WorldTransform = hlslpp::float4x4::identity();
        Uint32 MaterialID                           = 0;
        Uint32 EntityID                             = 0;
        Uint32 GeometryID                           = 0;
    };
    static_assert(sizeof(GpuData) == 80);
    static_assert(offsetof(GpuData, WorldTransform) == 0);
    static_assert(offsetof(GpuData, MaterialID) == 64);
    static_assert(offsetof(GpuData, EntityID) == 68);
    static_assert(offsetof(GpuData, GeometryID) == 72);

    [[nodiscard]] auto BuildGpuData(Uint32 GeometryID, Uint32 MaterialID) const -> GpuData {
        return GpuData{
            .WorldTransform = WorldTransform,
            .MaterialID     = MaterialID,
            .EntityID       = EntityId,
            .GeometryID     = GeometryID,
        };
    }
};

export struct GeometryLoader {
    using result_type = std::shared_ptr<GeometryRecord>;

    auto operator()(StringView MeshPath, Uint32 MeshIndex, const aiMesh* AiMesh) const -> result_type;
};

export using GeometryCache = entt::resource_cache<GeometryRecord, GeometryLoader>;

export struct MeshLoader {
    using result_type = std::shared_ptr<MeshRecord>;

    auto operator()(const Path& MeshPath) -> result_type;

    GeometryCache       m_GeometryCache;
    MaterialAssimpCache m_MaterialAssimpCache;
};

export using MeshCache = entt::resource_cache<MeshRecord, MeshLoader>;

/// @brief System for loading mesh assets and collecting render records.
export class MeshSystem : public ISystem {
  public:
    explicit MeshSystem(entt::registry& Registry) : ISystem(Registry) {}
    ~MeshSystem() override = default;

    auto SetAssetRoot(Path AssetRoot) -> void;

    auto OnUpdate(Float32) -> void override;

    [[nodiscard]] auto CollectInstances() const -> std::vector<InstanceRecord>;

    /// @brief Collect live read-only material handles cached by this Scene.
    /// @return YAML material handles followed by cached mesh material handles.
    [[nodiscard]] auto CollectMaterialRecords() const -> std::vector<ConstMaterialHandle>;

    [[nodiscard]] auto GetTextures() const -> const std::vector<RHIRef<RHITexture>>&;

  private:
    std::vector<RHIRef<RHITexture>> m_Textures  = {};
    Path                            m_AssetRoot = {};
    MeshCache                       m_MeshCache;
    MaterialYamlCache               m_MaterialYamlCache;
};

} // namespace SoulEngine
