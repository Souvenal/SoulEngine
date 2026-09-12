module;

#include <cstddef>
#include <hlsl++.h>

export module Renderer:Common;

import Core;
import Material;
import RHI;
import Scene;

export import std;

export namespace SoulEngine {

/// @brief Shared frame constant-buffer ABI used by every renderer.
struct alignas(16) RendererFrameConstants {
    Float32 Time          = 0.0f;
    Float32 ExposureEV100 = 15.0f;
    Uint32  LightCount    = 0;
};
static_assert(sizeof(RendererFrameConstants) == 16);
static_assert(offsetof(RendererFrameConstants, Time) == 0);
static_assert(offsetof(RendererFrameConstants, ExposureEV100) == 4);
static_assert(offsetof(RendererFrameConstants, LightCount) == 8);

/// @brief Shared view constant-buffer ABI used by every renderer.
struct alignas(16) RendererViewConstants {
    /// @brief Build the shared view constants from an immutable camera record.
    explicit RendererViewConstants(const CameraViewRecord& View)
        : ViewProjection(View.ViewProjection),
          ViewProjectionInverse(hlslpp::inverse(View.ViewProjection)),
          CameraPosition(hlslpp::interop::float4{
              hlslpp::float4{View.CameraPosition.x, View.CameraPosition.y, View.CameraPosition.z, 1.0f}}),
          ViewportSize(hlslpp::interop::float2{
              hlslpp::float2{static_cast<Float32>(View.GetWidth()), static_cast<Float32>(View.GetHeight())}}) {}

    alignas(16) hlslpp::float4x4 ViewProjection = hlslpp::float4x4::identity();
    alignas(16) hlslpp::float4x4 ViewProjectionInverse = hlslpp::float4x4::identity();
    alignas(16) hlslpp::interop::float4 CameraPosition = hlslpp::interop::float4{
        hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f}};
    hlslpp::interop::float2 ViewportSize = hlslpp::interop::float2{
        hlslpp::float2{0.0f, 0.0f}};
};
static_assert(sizeof(RendererViewConstants) == 160);
static_assert(offsetof(RendererViewConstants, ViewProjection) == 0);
static_assert(offsetof(RendererViewConstants, ViewProjectionInverse) == 64);
static_assert(offsetof(RendererViewConstants, CameraPosition) == 128);
static_assert(offsetof(RendererViewConstants, ViewportSize) == 144);

/// @brief Build the shared transient GPU light table from scene records.
[[nodiscard]] auto BuildLightGpuData(std::span<const LightRecord> Lights)
    -> std::vector<LightRecord::GpuData> {
    std::vector<LightRecord::GpuData> Result = {};
    Result.reserve(std::max<std::size_t>(Lights.size(), 1));
    for (const auto& Light : Lights)
        Result.emplace_back(Light.BuildGpuData());
    if (Result.empty())
        Result.emplace_back();
    return Result;
}

/// @brief Renderer-side authority for GeometryRecord GPU residency.
///
/// Lazy upload: the Scene loader fills CPU data only. One uploader instance
/// per renderer is the sole writer of its records' GPU state, and Upload is
/// its single mutation entry point: staged, idempotent, and meant to be
/// called once per frame while `IsUploadRequested(NeedBLAS)` is false.
/// Queries live on the record itself (IsUploadRequested / IsReadyOnGpu);
/// mutation is never a member operation.
///
/// Buffers are queued on the first call; the BLAS follows once the buffers
/// are Ready (the BLAS descriptor bakes their device addresses). A deferred
/// stage is normal progress, not an error; the caller retries next frame. A
/// failed stage leaves its flag unset, so the next call re-attempts exactly
/// that stage.
///
/// BLAS builds are collected, not executed: the BLAS stage creates the
/// hollow descriptor (synchronously — Create performs no device calls) and
/// enters it into the pending batch in the same step, guarded by
/// `BlasRequested` so each BLAS joins exactly once. The renderer drains the
/// batch at the end of Render() into RenderResult, and the device builds the
/// whole batch at the start of the frame's execution.
class GeometryUploader {
  public:
    GeometryUploader() = default;

    /// Issue the outstanding upload stages for a record. Buffers are queued on
    /// the first call; the BLAS stage (descriptor creation + pending-build
    /// batch entry, one step) follows once the buffers are Ready and NeedBLAS
    /// is set. Each stage checks its own flag, so a fully-requested record is
    /// a silent no-op; failures leave the stage's flag unset for the next
    /// call to retry.
    auto Upload(GeometryRecord& Record, bool NeedBLAS) -> std::expected<void, ErrorMessage> {
        if (!Record.BufferUploadRequested)
            if (auto R = UploadBuffers(Record); !R)
                return R;

        if (NeedBLAS && !Record.BlasRequested && Record.IsReadyOnGpu(false)) {
            if (auto R = UploadBlas(Record); !R)
                return R;
            // Geometries excluded at the gate below set BlasRequested without
            // producing a BLAS; they never join the pending build batch.
            if (Record.Blas)
                m_PendingBlasBuilds.push_back(Record.Blas);
        }

        return {};
    }

    /// Drain the pending BLAS build batch for this frame's RenderResult.
    [[nodiscard]] auto DrainPendingBlasBuilds()
        -> std::vector<RHIRef<RHIBottomLevelAccelerationStructure>> {
        return std::exchange(m_PendingBlasBuilds, {});
    }

  private:
    /// Queue vertex/index buffer creation. Failures leave the flag unset so a
    /// later frame retries.
    [[nodiscard]] static auto UploadBuffers(GeometryRecord& Record) -> std::expected<void, ErrorMessage> {
        if (Record.Positions.empty() || Record.Indices.empty())
            return std::unexpected(
                ErrorMessage(Format("GeometryUploader: '{}' has no CPU geometry to upload", Record.Name)));

        auto&      Device = RHIRenderDevice::Get();
        const auto Base   = Format("Geometry/{}", Record.Name.empty() ? String{"Unnamed"} : Record.Name);

        const auto CreateVertexBuffer = [&Device, &Base](StringView                 AttributeName,
                                                         std::span<const std::byte> Data,
                                                         Uint64 VertexCount,
                                                         Uint32 Stride)
            -> std::expected<RHIRef<RHIVertexBuffer>, ErrorMessage> {
            auto Result = Device.CreateVertexBuffer(Format("{}/{}", Base, AttributeName),
                                                    {
                                                        .Data        = Data,
                                                        .VertexCount = VertexCount,
                                                        .Stride      = Stride,
                                                    });
            if (!Result)
                return std::unexpected(Result.error().Append(
                    Format("GeometryUploader: {} buffer creation failed", AttributeName)));
            return std::move(*Result);
        };

        auto Position = CreateVertexBuffer("Position",
                                           std::as_bytes(std::span{Record.Positions}),
                                           Record.Positions.size(),
                                           sizeof(Record.Positions[0]));
        if (!Position)
            return std::unexpected(std::move(Position.error()));
        auto Normal = CreateVertexBuffer("Normal",
                                         std::as_bytes(std::span{Record.Normals}),
                                         Record.Normals.size(),
                                         sizeof(Record.Normals[0]));
        if (!Normal)
            return std::unexpected(std::move(Normal.error()));

        RHIRef<RHIVertexBuffer> Tangent = nullptr;
        if (Record.HasTangents && !Record.Tangents.empty()) {
            auto Result = CreateVertexBuffer("Tangent",
                                             std::as_bytes(std::span{Record.Tangents}),
                                             Record.Tangents.size(),
                                             sizeof(Record.Tangents[0]));
            if (!Result)
                return std::unexpected(std::move(Result.error()));
            Tangent = std::move(*Result);
        }

        RHIRef<RHIVertexBuffer> TexCoord = nullptr;
        if (Record.HasUV0 && !Record.UVs.empty()) {
            auto Result = CreateVertexBuffer("TexCoord",
                                             std::as_bytes(std::span{Record.UVs}),
                                             Record.UVs.size(),
                                             sizeof(Record.UVs[0]));
            if (!Result)
                return std::unexpected(std::move(Result.error()));
            TexCoord = std::move(*Result);
        }

        auto Index = Device.CreateIndexBuffer(Format("{}/Index", Base),
                                              {
                                                  .Data       = std::as_bytes(std::span{Record.Indices}),
                                                  .IndexCount = Record.Indices.size(),
                                              });
        if (!Index)
            return std::unexpected(Index.error().Append("GeometryUploader: index buffer creation failed"));

        Record.PositionBuffer        = std::move(*Position);
        Record.NormalBuffer          = std::move(*Normal);
        Record.TangentBuffer         = std::move(Tangent);
        Record.TexCoordBuffer        = std::move(TexCoord);
        Record.IndexBuffer           = std::move(*Index);
        Record.BufferUploadRequested = true;
        return {};
    }

    /// Queue the geometry-owned static BLAS descriptor (hollow until its
    /// deferred first build materializes storage at the frame-start build
    /// batch). Caller guarantees the buffers are Ready. Failures leave the
    /// flag unset so a later frame retries.
    [[nodiscard]] static auto UploadBlas(GeometryRecord& Record) -> std::expected<void, ErrorMessage> {
        // Ray-tracing gate: a BLAS needs at least one complete triangle. A
        // geometry failing it is excluded exactly once — BlasRequested stays
        // set while Record.Blas stays null — so it never joins the pending
        // build batch and stays invisible to ray tracing; raster rendering is
        // unaffected. Downstream (hollow Create aside) the Vulkan batch trusts
        // this gate by comment instead of re-validating or asserting.
        if (Record.Indices.empty() || Record.Indices.size() % 3 != 0) {
            LogWarning("GeometryUploader: '{}' has no complete triangles; excluded from ray tracing BLAS builds",
                       Record.Name.empty() ? String{"Unnamed"} : Record.Name);
            Record.BlasRequested = true;
            return {};
        }
        const RHIBottomLevelAccelerationStructureDesc BlasDesc{
            .VertexBufferRef = Record.PositionBuffer,
            .IndexBufferRef  = Record.IndexBuffer,
        };
        auto Blas = RHIRenderDevice::Get().CreateBottomLevelAccelerationStructure(
            Format("BLAS/Geometry/{}", Record.Name.empty() ? String{"Unnamed"} : Record.Name), BlasDesc);
        if (!Blas)
            return std::unexpected(Blas.error().Append(
                Format("GeometryUploader: BLAS allocation failed for '{}'", Record.Name)));

        Record.Blas          = std::move(*Blas);
        Record.BlasRequested = true;
        return {};
    }

    std::vector<RHIRef<RHIBottomLevelAccelerationStructure>> m_PendingBlasBuilds = {};
};

/// @brief Per-frame renderer-neutral GPU table bundle built from the
/// renderer-filtered InstanceRecord values.
///
/// Holds the shared GPU-ABI tables (instances, geometries, materials, lights)
/// plus two per-geometry parallel columns: GeometryIndexCounts (sourced from
/// the RHI index buffer, consumed by the raster indirect-command derivation)
/// and GeometryBlas (the geometry-owned BLAS refs, consumed by the RT TLAS
/// derivation; null on the raster path where BLAS is never requested).
/// Lights take no filter or upload path: the full scene light list is baked
/// once per frame and shared by every view.
///
/// Create is pure table building — upload and readiness gating are the
/// renderer's own filter step ahead of it. Instance order is canonical:
/// sorted by GeometryID, so the raster draw grouping and the RT TLAS slot
/// order follow the same sequence. Material slot 0 is always the default
/// entry. Geometry and material are deliberately unbound here: the material
/// is resolved through the instance record, never through the geometry.
struct SceneGpuData {
    std::vector<InstanceRecord::GpuData> InstanceRecords = {};
    std::vector<GeometryRecord::GpuData> GeometryRecords = {};
    std::vector<MaterialRecord::GpuData> MaterialRecords = {};
    std::vector<LightRecord::GpuData>    LightRecords   = {};
    std::vector<Uint32>                  GeometryIndexCounts = {};
    std::vector<RHIRef<RHIBottomLevelAccelerationStructure>> GeometryBlas = {};

    [[nodiscard]] static auto Create(std::span<const InstanceRecord>        Instances,
                                     std::span<const LightRecord>           Lights,
                                     const RHIRefArray<RHISampledTexture>& Textures) -> SceneGpuData {
        SceneGpuData Result = {};
        std::unordered_map<const GeometryRecord*, Uint32> GeometryIDs = {};
        std::unordered_map<const MaterialRecord*, Uint32> MaterialIDs = {};
        GeometryIDs.reserve(Instances.size());
        MaterialIDs.reserve(Instances.size());
        Result.InstanceRecords.reserve(Instances.size());
        // Deferred lighting samples material index 0 even for an empty scene.
        Result.MaterialRecords.emplace_back(MaterialRecord::GpuData{});
        Result.LightRecords = BuildLightGpuData(Lights);

        for (const auto& SourceInstance : Instances) {
            // The renderer's filter step guarantees a non-null, GPU-ready
            // geometry on every instance it accepts.
            auto&      Geometry    = *SourceInstance.Geometry;
            const auto* GeometryPtr = std::addressof(Geometry);
            const auto  [It, Inserted] =
                GeometryIDs.emplace(GeometryPtr, static_cast<Uint32>(Result.GeometryRecords.size()));
            const auto GeometryID = It->second;
            if (Inserted) {
                Result.GeometryRecords.emplace_back(Geometry.BuildGpuData());
                Result.GeometryIndexCounts.emplace_back(
                    static_cast<Uint32>(Geometry.IndexBuffer->GetIndexCount()));
                Result.GeometryBlas.emplace_back(Geometry.Blas);
            }
            Uint32 MaterialID = 0;
            if (SourceInstance.Material) {
                const auto* Record = std::addressof(*SourceInstance.Material);
                const auto [MaterialIt, MaterialInserted] =
                    MaterialIDs.emplace(Record, static_cast<Uint32>(Result.MaterialRecords.size()));
                MaterialID = MaterialIt->second;
                if (MaterialInserted) {
                    Result.MaterialRecords.emplace_back(Record->BuildGpuData(Textures));
                }
            }
            Result.InstanceRecords.emplace_back(SourceInstance.BuildGpuData(GeometryID, MaterialID));
        }

        std::ranges::sort(Result.InstanceRecords, [](const auto& Lhs, const auto& Rhs) {
            return Lhs.GeometryID < Rhs.GeometryID;
        });

        return Result;
    }
};

} // namespace SoulEngine
