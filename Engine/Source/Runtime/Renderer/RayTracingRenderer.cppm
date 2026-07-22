module;

// needed for offsetof
#include <cstddef>
#include <hlsl++.h>

export module Renderer:RayTracingRenderer;

import Core;
import RHI;
import Resource;
import Scene;

import :IRenderer;

export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Renderer {

constexpr Uint32 kMaxRayTracingInstances = 64;
constexpr Uint32 kPathTracingMaxBounces = 6;

/// Constant-buffer ABI mirror for RayTracing.slang camera unprojection data.
struct alignas(16) RayTracingViewConstants {
    alignas(16) hlslpp::float4x4 ViewProjectionInverse = hlslpp::float4x4::identity();
    alignas(16) hlslpp::interop::float4 CameraPosition =
        hlslpp::interop::float4{hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f}};
    alignas(16) hlslpp::interop::float4 DirectionalLightDirectionIntensity =
        hlslpp::interop::float4{hlslpp::float4{-0.4f, -1.0f, -0.8f, 5.0f}};
    alignas(16) hlslpp::interop::float4 DirectionalLightColor =
        hlslpp::interop::float4{hlslpp::float4{1.0f, 0.98f, 0.92f, 1.0f}};
    alignas(16) hlslpp::interop::float4 PathSettings =
        hlslpp::interop::float4{hlslpp::float4{0.0f, static_cast<float>(kPathTracingMaxBounces), 0.0f, 0.0f}};
};
static_assert(sizeof(RayTracingViewConstants) == 128,
              "RayTracingViewConstants must match RayTracing.slang RayTracingViewData std140 layout");
static_assert(offsetof(RayTracingViewConstants, ViewProjectionInverse) == 0,
              "RayTracingViewConstants::ViewProjectionInverse must match RayTracingViewData.viewProjectionInverse");
static_assert(offsetof(RayTracingViewConstants, CameraPosition) == 64,
              "RayTracingViewConstants::CameraPosition must match RayTracingViewData.cameraPosition");
static_assert(offsetof(RayTracingViewConstants, PathSettings) == 112,
              "RayTracingViewConstants::PathSettings must match RayTracingViewData.pathSettings");

struct alignas(16) RayTracingMaterialConstants {
    alignas(16) std::array<hlslpp::interop::float4, kMaxRayTracingInstances> BaseColorMetallic = {};
    alignas(16) std::array<hlslpp::interop::float4, kMaxRayTracingInstances> RoughnessGeometryBase = {};
};
static_assert(sizeof(RayTracingMaterialConstants) == kMaxRayTracingInstances * 32,
              "RayTracingMaterialConstants must match RayTracing.slang RayTracingMaterialData std140 layout");
static_assert(offsetof(RayTracingMaterialConstants, BaseColorMetallic) == 0,
              "RayTracingMaterialConstants::BaseColorMetallic must match RayTracingMaterialData.baseColorMetallic");
static_assert(offsetof(RayTracingMaterialConstants, RoughnessGeometryBase) == kMaxRayTracingInstances * 16,
              "RayTracingMaterialConstants::RoughnessGeometryBase must match RayTracingMaterialData.roughnessGeometryBase");

struct RayTracingMeshCacheEntry {
    String Asset = {};
    Resource::ResourceRef<Resource::Mesh> Mesh = {};
    Resource::ResourceRef<Resource::BottomLevelAccelerationStructure> Blas = {};
};

/// @brief Convert a Scene row-vector transform to an RHI TLAS instance transform.
///
/// RHI TLAS transforms are row-major 3x4 matrices with translation in the
/// final column. Scene matrices are row-vector transforms, so the upper-left
/// 3x3 block is transposed while the fourth row becomes translation.
[[nodiscard]] auto ToAccelerationStructureInstanceTransform(const hlslpp::float4x4& Matrix)
    -> RHI::RowMajorTransform3x4 {
    return {
        .M00 = Matrix[0].x, .M01 = Matrix[1].x, .M02 = Matrix[2].x, .M03 = Matrix[3].x,
        .M10 = Matrix[0].y, .M11 = Matrix[1].y, .M12 = Matrix[2].y, .M13 = Matrix[3].y,
        .M20 = Matrix[0].z, .M21 = Matrix[1].z, .M22 = Matrix[2].z, .M23 = Matrix[3].z,
    };
}

/// First SceneSnapshot-driven hardware ray-tracing renderer.
class RayTracingRenderer final : public IRenderer {
  public:
    RayTracingRenderer() = default;
    ~RayTracingRenderer() override { OnDetach(); }

    [[nodiscard]] auto OnAttach() -> std::expected<void, ErrorMessage> override {
        const auto ShaderPath = ConfigManager::Get().EngineShadersDirPath() / "RayTracing.slang";
        auto& Resources = Resource::Manager::Get();
        m_Pipeline = Resources.RequestRayTracingPipelineRef(Resource::RayTracingPipelineRequest{
            .RayGeneration = {.SourcePath = ShaderPath, .EntryPoint = "rayGenMain"},
            .MissEntries = {
                {.SourcePath = ShaderPath, .EntryPoint = "missMain"},
                {.SourcePath = ShaderPath, .EntryPoint = "shadowMissMain"},
            },
            .HitGroups = {
                {.Type = Shader::RayTracingHitGroupType::Triangles,
                 .ClosestHit = ShaderCompiler::ShaderEntry{.SourcePath = ShaderPath, .EntryPoint = "closestHitMain"}},
                {.Type = Shader::RayTracingHitGroupType::Triangles,
                 .ClosestHit = ShaderCompiler::ShaderEntry{.SourcePath = ShaderPath, .EntryPoint = "shadowClosestHitMain"}},
            },
        });
        m_Tlas = Resources.RequestTopLevelAccelerationStructureRef(
            "ray_tracing_renderer_main", {.InitialInstanceCapacity = 16,
                                            .BuildFlags = RHI::AccelerationStructureBuildFlags::AllowUpdate});
        m_ViewConstants = Resources.RequestConstantBufferRef(
            "ray_tracing_renderer_view_constants", {.Size = sizeof(RayTracingViewConstants)});
        m_MaterialConstants = Resources.RequestConstantBufferRef(
            "ray_tracing_renderer_material_constants", {.Size = sizeof(RayTracingMaterialConstants)});
        if (!m_Pipeline || !m_Tlas || !m_ViewConstants || !m_MaterialConstants)
            return std::unexpected(ErrorMessage("RayTracingRenderer resource request failed"));
        m_GeometryTable = RHI::RenderDevice::Get().GetRayTracingGeometryTable();
        if (!m_GeometryTable)
            return std::unexpected(ErrorMessage("RayTracingRenderer requires Vulkan BDA geometry-table support"));
        return {};
    }

    auto OnDetach() -> void override {
        m_Pipeline = {};
        m_Tlas = {};
        m_ViewConstants = {};
        m_MaterialConstants = {};
        m_GeometryTable = nullptr;
        m_Output = {};
        m_Accumulation = {};
        m_OutputKey = {};
        m_AccumulationKey = {};
        m_MeshCache.clear();
        m_Parameters = {};
        m_LastSceneSignature = 0;
        m_SampleIndex = 0;
        m_HasAccumulation = false;
        m_LoggedFirstTrace = false;
    }

    [[nodiscard]] auto Render(const Scene::SceneSnapshot& Scene) -> std::expected<RenderResult, ErrorMessage> override {
        RenderResult Result = {};
        if (Scene.Views.empty())
            return Result;

        auto& Resources = Resource::Manager::Get();
        auto* Pipeline = Resources.TryGetReady(m_Pipeline);
        auto* Tlas = Resources.TryGetReady(m_Tlas);
        if (!Pipeline || !Tlas)
            return Result;
        if (!m_GeometryTable)
            return std::unexpected(ErrorMessage("RayTracingRenderer BDA geometry table is unavailable"));

        std::vector<RHI::AccelerationStructureInstance> Instances = {};
        Instances.reserve(Scene.Renderables.size());
        RHI::RayTracingGeometryTableUpdate GeometryUpdate = {};
        GeometryUpdate.Instances.reserve(Scene.Renderables.size());
        RayTracingMaterialConstants MaterialData = {};
        for (const auto& Renderable : Scene.Renderables) {
            if (Renderable.MeshAsset.empty())
                continue;

            auto& Entry = GetOrRequestMeshEntry(Renderable.MeshAsset);
            auto* Blas = Resources.TryGetReady(Entry.Blas);
            auto* Mesh = Resources.TryGetReady(Entry.Mesh);
            if (!Blas || !Blas->GetRhiPayload() || !Mesh)
                continue;

            std::vector<RHI::RayTracingGeometryDesc> MeshGeometries = {};
            for (const auto& Group : Mesh->GetMeshGroups()) {
                for (const auto& SubMesh : Group.SubMeshes) {
                    auto* Position = Resources.TryGetReady(SubMesh.PositionVB);
                    auto* Normal = Resources.TryGetReady(SubMesh.NormalVB);
                    auto* Index = Resources.TryGetReady(SubMesh.IB);
                    if (!Position || !Normal || !Index || SubMesh.VertexCount == 0 || SubMesh.Indices.empty()) {
                        MeshGeometries.clear();
                        break;
                    }
                    MeshGeometries.push_back(RHI::RayTracingGeometryDesc{
                        .PositionBuffer = Position,
                        .NormalBuffer = Normal,
                        .IndexBuffer = Index,
                        .PositionStride = sizeof(hlslpp::interop::float3),
                        .NormalStride = sizeof(hlslpp::interop::float3),
                        .IndexStride = sizeof(Uint32),
                        .VertexCount = SubMesh.VertexCount,
                        .IndexCount = static_cast<Uint32>(SubMesh.Indices.size()),
                    });
                }
                if (MeshGeometries.empty())
                    break;
            }
            if (MeshGeometries.empty())
                continue;
            if (Instances.size() >= kMaxRayTracingInstances) {
                return std::unexpected(ErrorMessage(Format(
                    "RayTracingRenderer supports at most {} ready scene instances per frame",
                    kMaxRayTracingInstances)));
            }

            const auto InstanceIndex = static_cast<Uint32>(Instances.size());
            const auto FirstGeometry = static_cast<Uint32>(GeometryUpdate.Geometries.size());
            GeometryUpdate.Geometries.insert(
                GeometryUpdate.Geometries.end(), MeshGeometries.begin(), MeshGeometries.end());
            GeometryUpdate.Instances.push_back(RHI::RayTracingGeometryInstanceDesc{
                .FirstGeometry = FirstGeometry,
                .GeometryCount = static_cast<Uint32>(MeshGeometries.size()),
                .MaterialIndex = InstanceIndex,
            });

            const auto& Material = Renderable.Material;
            MaterialData.BaseColorMetallic[InstanceIndex] = hlslpp::interop::float4{
                hlslpp::float4{Material.BaseColor.x, Material.BaseColor.y, Material.BaseColor.z, Material.Metallic}};
            MaterialData.RoughnessGeometryBase[InstanceIndex] = hlslpp::interop::float4{
                hlslpp::float4{Material.Roughness, 0.0f, 0.0f, 0.0f}};
            Instances.push_back(RHI::AccelerationStructureInstance{
                .BottomLevelPtr = Blas->GetRhiPayload(),
                .Transform = ToAccelerationStructureInstanceTransform(Renderable.WorldTransform),
                .CustomIndex = InstanceIndex,
            });
        }
        if (Instances.empty())
            return Result;

        const auto& View = Scene.Views.front();
        auto* ViewOutput = Resources.TryGetReady(View.ColorRT);
        if (!ViewOutput)
            return Result;
        const bool bTargetsChanged = EnsureOutputTargets(Resources, ViewOutput->GetWidth(), ViewOutput->GetHeight());
        auto* Output = Resources.TryGetReady(m_Output);
        auto* Accumulation = Resources.TryGetReady(m_Accumulation);
        auto* ViewCB = Resources.TryGetReady(m_ViewConstants);
        auto* MaterialCB = Resources.TryGetReady(m_MaterialConstants);
        if (!Output || !Accumulation || !ViewCB || !MaterialCB)
            return Result;

        const auto SceneSignature = BuildSceneSignature(Scene, View, Instances.size(), GeometryUpdate.Geometries.size());
        if (bTargetsChanged || !m_HasAccumulation || SceneSignature != m_LastSceneSignature) {
            m_SampleIndex = 0;
            m_LastSceneSignature = SceneSignature;
            m_HasAccumulation = true;
        }

        if (m_Parameters.GetLayoutId() != Pipeline->GetShaderParameterLayout().GetId())
            m_Parameters = RHI::ShaderParameters::Create(*Pipeline);
        if (auto R = m_Parameters.SetTopLevelAccelerationStructure("g_rayTracing.tlas", Tlas->GetRhiPayload()); !R)
            return std::unexpected(R.error().Append("RayTracingRenderer TLAS parameter binding failed"));
        if (auto R = m_Parameters.SetStorageRenderTarget("g_rayTracing.output", Output); !R)
            return std::unexpected(R.error().Append("RayTracingRenderer output parameter binding failed"));
        if (auto R = m_Parameters.SetStorageRenderTarget("g_rayTracing.accumulation", Accumulation); !R)
            return std::unexpected(R.error().Append("RayTracingRenderer accumulation parameter binding failed"));
        if (auto R = m_Parameters.SetRayTracingGeometryTable("g_rayTracingGeometryMetadata.metadata", m_GeometryTable); !R)
            return std::unexpected(R.error().Append("RayTracingRenderer BDA geometry-table parameter binding failed"));
        if (auto R = m_Parameters.SetConstantBuffer(
                "g_rayTracing.materials", MaterialCB, &MaterialData, sizeof(MaterialData));
            !R) {
            return std::unexpected(R.error().Append("RayTracingRenderer material parameter binding failed"));
        }
        const auto ViewData = BuildViewConstants(View, m_SampleIndex);
        if (auto R = m_Parameters.SetConstantBuffer("g_rayTracing.view", ViewCB, &ViewData, sizeof(ViewData)); !R)
            return std::unexpected(R.error().Append("RayTracingRenderer view parameter binding failed"));

        RHI::NonRenderingPass Pass = {};
        Pass.UpdateRayTracingGeometryTable(m_GeometryTable, std::move(GeometryUpdate));
        Pass.BuildOrUpdateTopLevelAccelerationStructure(Tlas->GetRhiPayload(), Instances);
        Pass.SetRayTracingPipeline(Pipeline);
        Pass.BindShaderParameters(Pipeline, m_Parameters);
        Pass.TraceRays(Pipeline, Output->GetWidth(), Output->GetHeight());
        Result.CmdList.Scopes.push_back(std::move(Pass));
        Result.CmdList.PresentSource = Output;
        ++m_SampleIndex;
        if (!m_LoggedFirstTrace) {
            LogInfo("RayTracingRenderer emitted progressive path tracing for {} instances at {}x{}",
                    Instances.size(), Output->GetWidth(), Output->GetHeight());
            m_LoggedFirstTrace = true;
        }
        return Result;
    }

  private:
    [[nodiscard]] auto GetOrRequestMeshEntry(StringView Asset) -> RayTracingMeshCacheEntry& {
        for (auto& Entry : m_MeshCache) {
            if (Entry.Asset == Asset)
                return Entry;
        }

        auto& Entry = m_MeshCache.emplace_back(RayTracingMeshCacheEntry{
            .Asset = String(Asset),
            .Mesh = Resource::Manager::Get().RequestMeshRef(ResolveMeshPath(Asset).string()),
        });
        Entry.Blas = Resource::Manager::Get().RequestBottomLevelAccelerationStructureRef(Entry.Mesh);
        return Entry;
    }

    auto EnsureOutputTargets(Resource::Manager& Resources, Uint32 Width, Uint32 Height) -> bool {
        const auto OutputKey = Format("ray_tracing_output_{}x{}", Width, Height);
        const auto AccumulationKey = Format("ray_tracing_accumulation_{}x{}", Width, Height);
        if (m_OutputKey != OutputKey || m_AccumulationKey != AccumulationKey) {
            m_Output = {};
            m_Accumulation = {};
            m_OutputKey = OutputKey;
            m_AccumulationKey = AccumulationKey;
        }
        if (!m_Output) {
            m_Output = Resources.RequestRenderTargetRef(
                OutputKey,
                RHI::RenderTargetDesc{
                    .Width = Width,
                    .Height = Height,
                    .Format = RHI::Format::B8G8R8A8_UNORM,
                    .Usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::ShaderStorage |
                             RHI::TextureUsage::FrameOutput,
                });
            return true;
        }
        if (!Resources.TryGetReady(m_Output))
            return true;
        if (!m_Accumulation) {
            m_Accumulation = Resources.RequestRenderTargetRef(
                AccumulationKey,
                RHI::RenderTargetDesc{
                    .Width = Width,
                    .Height = Height,
                    .Format = RHI::Format::R32G32B32A32_SFLOAT,
                    .Usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::ShaderStorage,
                });
            return true;
        }
        return false;
    }

    [[nodiscard]] static auto ResolveMeshPath(StringView AssetPath) -> Path {
        const Path Candidate{String(AssetPath)};
        if (Candidate.is_absolute())
            return Candidate;
        return (ConfigManager::Get().CurrentApplicationDir() / "Assets" / Candidate).lexically_normal();
    }

    [[nodiscard]] static auto HashCombine(Uint64 Seed, Uint64 Value) -> Uint64 {
        return Seed ^ (Value + 0x9e3779b97f4a7c15ULL + (Seed << 6) + (Seed >> 2));
    }

    [[nodiscard]] static auto HashFloat(float Value) -> Uint64 {
        return std::bit_cast<Uint32>(Value);
    }

    [[nodiscard]] static auto HashMatrix(Uint64 Seed, const hlslpp::float4x4& Matrix) -> Uint64 {
        for (Uint32 Row = 0; Row < 4; ++Row) {
            Seed = HashCombine(Seed, HashFloat(Matrix[Row].x));
            Seed = HashCombine(Seed, HashFloat(Matrix[Row].y));
            Seed = HashCombine(Seed, HashFloat(Matrix[Row].z));
            Seed = HashCombine(Seed, HashFloat(Matrix[Row].w));
        }
        return Seed;
    }

    [[nodiscard]] static auto BuildSceneSignature(const Scene::SceneSnapshot& Scene,
                                                   const Scene::RenderViewSnapshot& View,
                                                   std::size_t InstanceCount,
                                                   std::size_t GeometryCount) -> Uint64 {
        Uint64 Signature = HashMatrix(1469598103934665603ULL, View.ViewProjection);
        Signature = HashCombine(Signature, InstanceCount);
        Signature = HashCombine(Signature, GeometryCount);
        for (const auto& Renderable : Scene.Renderables) {
            Signature = HashCombine(Signature, std::hash<String>{}(Renderable.MeshAsset));
            Signature = HashMatrix(Signature, Renderable.WorldTransform);
            Signature = HashCombine(Signature, HashFloat(Renderable.Material.BaseColor.x));
            Signature = HashCombine(Signature, HashFloat(Renderable.Material.BaseColor.y));
            Signature = HashCombine(Signature, HashFloat(Renderable.Material.BaseColor.z));
            Signature = HashCombine(Signature, HashFloat(Renderable.Material.Metallic));
            Signature = HashCombine(Signature, HashFloat(Renderable.Material.Roughness));
        }
        return Signature;
    }

    [[nodiscard]] static auto BuildViewConstants(const Scene::RenderViewSnapshot& View, Uint32 SampleIndex)
        -> RayTracingViewConstants {
        return RayTracingViewConstants{
            .ViewProjectionInverse = hlslpp::inverse(View.ViewProjection),
            .CameraPosition = hlslpp::interop::float4{
                hlslpp::float4{View.CameraPosition.x, View.CameraPosition.y, View.CameraPosition.z, 1.0f}},
            .PathSettings = hlslpp::interop::float4{
                hlslpp::float4{static_cast<float>(SampleIndex), static_cast<float>(kPathTracingMaxBounces), 0.0f, 0.0f}},
        };
    }

    Resource::ResourceRef<RHI::RayTracingPipeline> m_Pipeline = {};
    Resource::ResourceRef<Resource::TopLevelAccelerationStructure> m_Tlas = {};
    Resource::ResourceRef<RHI::ConstantBuffer> m_ViewConstants = {};
    Resource::ResourceRef<RHI::ConstantBuffer> m_MaterialConstants = {};
    RHI::RayTracingGeometryTable* m_GeometryTable = nullptr;
    Resource::ResourceRef<RHI::RenderTarget> m_Output = {};
    Resource::ResourceRef<RHI::RenderTarget> m_Accumulation = {};
    String m_OutputKey = {};
    String m_AccumulationKey = {};
    std::vector<RayTracingMeshCacheEntry> m_MeshCache = {};
    RHI::ShaderParameters m_Parameters = {};
    Uint64 m_LastSceneSignature = 0;
    Uint32 m_SampleIndex = 0;
    bool m_HasAccumulation = false;
    bool m_LoggedFirstTrace = false;
};

RendererFactory::AutoRegistrar<RayTracingRenderer> RegRayTracingRenderer{"RayTracing"};

} // namespace SoulEngine::Renderer