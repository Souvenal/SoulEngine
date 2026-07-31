module;

// needed for offsetof
#include <cstddef>
#include <hlsl++.h>

export module Renderer:RayTracingRenderer;

import Core;
import Material;
import RHI;
import Resource;
import Scene;
import TaskGraph;

import :IRenderer;
import :MaterialResolver;

export import std;

export namespace SoulEngine {

constexpr Uint32 kPathTracingMaxBounces = 6;

/// Constant-buffer ABI mirror for RayTracing.slang camera unprojection data.
struct alignas(16) RayTracingViewConstants {
    alignas(16) hlslpp::float4x4 ViewProjectionInverse = hlslpp::float4x4::identity();
    alignas(16) hlslpp::interop::float4 CameraPosition = hlslpp::interop::float4{
        hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f}};
    alignas(16) hlslpp::interop::float4 DirectionalLightDirectionIntensity = hlslpp::interop::float4{
        hlslpp::float4{-0.4f, -1.0f, -0.8f, 5.0f}};
    alignas(16) hlslpp::interop::float4 DirectionalLightColor = hlslpp::interop::float4{
        hlslpp::float4{1.0f, 0.98f, 0.92f, 1.0f}};
    alignas(16) hlslpp::interop::float4 PathSettings = hlslpp::interop::float4{
        hlslpp::float4{0.0f, static_cast<float>(kPathTracingMaxBounces), 0.0f, 0.0f}};
};
static_assert(sizeof(RayTracingViewConstants) == 128,
              "RayTracingViewConstants must match RayTracing.slang RayTracingViewData std140 layout");
static_assert(offsetof(RayTracingViewConstants, ViewProjectionInverse) == 0,
              "RayTracingViewConstants::ViewProjectionInverse must match RayTracingViewData.viewProjectionInverse");
static_assert(offsetof(RayTracingViewConstants, CameraPosition) == 64,
              "RayTracingViewConstants::CameraPosition must match RayTracingViewData.cameraPosition");
static_assert(offsetof(RayTracingViewConstants, PathSettings) == 112,
              "RayTracingViewConstants::PathSettings must match RayTracingViewData.pathSettings");

/// @brief Convert a Scene row-vector transform to an RHI TLAS instance transform.
///
/// RHI TLAS transforms are row-major 3x4 matrices with translation in the
/// final column. Scene matrices are row-vector transforms, so the upper-left
/// 3x3 block is transposed while the fourth row becomes translation.
[[nodiscard]] auto ToAccelerationStructureInstanceTransform(const hlslpp::float4x4& Matrix) -> RHIRowMajorTransform3x4 {
    return {
        .M00 = Matrix[0].x,
        .M01 = Matrix[1].x,
        .M02 = Matrix[2].x,
        .M03 = Matrix[3].x,
        .M10 = Matrix[0].y,
        .M11 = Matrix[1].y,
        .M12 = Matrix[2].y,
        .M13 = Matrix[3].y,
        .M20 = Matrix[0].z,
        .M21 = Matrix[1].z,
        .M22 = Matrix[2].z,
        .M23 = Matrix[3].z,
    };
}

/// First SceneSnapshot-driven hardware ray-tracing renderer.
class RayTracingRenderer final : public IRenderer {
  public:
    RayTracingRenderer() = default;
    ~RayTracingRenderer() override {
        OnDetach();
    }

    [[nodiscard]] auto OnAttach() -> std::expected<void, ErrorMessage> override {
        const auto ShaderPath = ConfigManager::Get().EngineShadersDirPath() / "RayTracing.slang";
        auto&      Resources  = ResourceManager::Get();
        auto PipelineRequest  = SubmitRayTracingPipelinePreparation(
            RayTracingPipelineRequest{
                .RayGeneration = {.SourcePath = ShaderPath, .EntryPoint = "rayGenMain"},
                .MissEntries =
                    {
                        {.SourcePath = ShaderPath, .EntryPoint = "missMain"},
                        {.SourcePath = ShaderPath, .EntryPoint = "shadowMissMain"},
                    },
                .HitGroups =
                    {
                        {.Type       = ShaderRayTracingHitGroupType::Triangles,
                         .ClosestHit = ShaderEntry{.SourcePath = ShaderPath, .EntryPoint = "closestHitMain"}},
                        {.Type       = ShaderRayTracingHitGroupType::Triangles,
                         .ClosestHit = ShaderEntry{.SourcePath = ShaderPath, .EntryPoint = "shadowClosestHitMain"}},
                    },
            },
            [this](RHIRef<RHIRayTracingPipeline> Pipeline) { m_Pipeline = std::move(Pipeline); });
        m_Tlas          = Resources.RequestTopLevelAccelerationStructureRef("ray_tracing_renderer_main",
                                                                            {.InitialInstanceCapacity = 16});
        m_SamplerLinear = RequestSampler({.Profile = RHISamplerProfile::LinearRepeat});
        m_SamplerAniso  = RequestSampler({.Profile = RHISamplerProfile::AnisotropicRepeat});
        if (!PipelineRequest || !m_Tlas || !m_SamplerLinear || !m_SamplerAniso)
            return std::unexpected(PipelineRequest
                                       ? ErrorMessage("RayTracingRenderer resource request failed")
                                       : PipelineRequest.error().Append("RayTracingRenderer pipeline request failed"));
        return {};
    }

    auto OnDetach() -> void override {
        m_Pipeline      = {};
        m_Tlas          = {};
        m_SamplerLinear = {};
        m_SamplerAniso  = {};
        m_MaterialResolver.Clear();
        m_Output             = {};
        m_Accumulation       = {};
        m_OutputKey          = {};
        m_AccumulationKey    = {};
        m_Parameters         = {};
        m_LastSceneSignature = 0;
        m_SampleIndex        = 0;
        m_HasAccumulation    = false;
    }

    [[nodiscard]] auto Render(const SceneSnapshot& Scene) -> std::expected<RenderResult, ErrorMessage> override {
        RenderResult Result = {};
        if (Scene.Views.empty())
            return Result;

        auto& Resources        = ResourceManager::Get();
        auto  PipelineRef      = m_Pipeline;
        auto* Pipeline         = PipelineRef.TryGet();
        auto* Tlas             = Resources.TryGetReady(m_Tlas);
        auto  TlasRef          = Tlas ? Tlas->GetRhiPayloadRef() : RHIRef<RHITopLevelAccelerationStructure>{nullptr};
        auto* TlasPayload      = TlasRef.TryGet();
        auto  SamplerLinearRef = m_SamplerLinear;
        auto  SamplerAnisoRef  = m_SamplerAniso;
        auto* SamplerLinear    = SamplerLinearRef.TryGet();
        auto* SamplerAniso     = SamplerAnisoRef.TryGet();
        if (!Pipeline || !Tlas || !TlasPayload || !SamplerLinear || !SamplerAniso)
            return Result;

        std::vector<RHIAccelerationStructureInstance> Instances = {};
        Instances.reserve(Scene.Renderables.size());
        std::vector<RHIRayTracingInstanceData> GeometryInstances = {};
        GeometryInstances.reserve(Scene.Renderables.size());
        std::vector<RHIRayTracingGeometryDesc> GeometrySources = {};
        m_MaterialResolver.BeginFrame();
        for (const auto& Renderable : Scene.Renderables) {
            if (Renderable.MeshAsset.empty())
                continue;

            auto  MeshRef        = Resources.RequestMeshRef(Renderable.MeshAsset);
            auto  BlasRef        = Resources.RequestBottomLevelAccelerationStructureRef(MeshRef);
            auto* Blas           = Resources.TryGetReady(BlasRef);
            auto  BlasPayloadRef = Blas ? Blas->GetRhiPayloadRef() : RHIRef<RHIBottomLevelAccelerationStructure>{nullptr};
            auto* BlasPayload    = BlasPayloadRef.TryGet();
            auto* Mesh           = Resources.TryGetReady(MeshRef);
            if (!Blas || !BlasPayload || !Mesh)
                continue;

            std::vector<RHIRayTracingGeometryDesc> MeshGeometries = {};
            for (const auto& Group : Mesh->GetMeshGroups()) {
                for (const auto& SubMesh : Group.SubMeshes) {
                    auto       PositionRef      = SubMesh.PositionVB;
                    auto       NormalRef        = SubMesh.NormalVB;
                    auto       TangentRef       = SubMesh.TangentVB;
                    auto       TexCoordRef      = SubMesh.UVVB;
                    auto       IndexRef         = SubMesh.IB;
                    auto*      Position         = PositionRef.TryGet();
                    auto*      Normal           = NormalRef.TryGet();
                    auto*      Tangent          = TangentRef.TryGet();
                    auto*      TexCoord         = TexCoordRef.TryGet();
                    auto*      Index            = IndexRef.TryGet();
                    const bool HasReadyTangents = SubMesh.HasTangents && Tangent != nullptr;
                    const bool HasReadyUV0      = SubMesh.HasUV0 && TexCoord != nullptr;
                    if (!Tangent) {
                        TangentRef = PositionRef;
                        Tangent    = Position;
                    }
                    if (!TexCoord) {
                        TexCoordRef = PositionRef;
                        TexCoord    = Position;
                    }
                    if (!Position || !Normal || !Tangent || !TexCoord || !Index || SubMesh.VertexCount == 0 ||
                        SubMesh.Indices.empty()) {
                        MeshGeometries.clear();
                        break;
                    }
                    const auto* ImportedMaterial =
                        Renderable.MaterialId.empty() ? Mesh->GetImportedMaterial(SubMesh.MaterialSlot) : nullptr;
                    const auto& Material = ImportedMaterial ? *ImportedMaterial : Renderable.Material;
                    MeshGeometries.push_back(RHIRayTracingGeometryDesc{
                        .PositionBufferRef = PositionRef,
                        .NormalBufferRef   = NormalRef,
                        .TangentBufferRef  = TangentRef,
                        .TexCoordBufferRef = TexCoordRef,
                        .IndexBufferRef    = IndexRef,
                        .PositionStride    = sizeof(hlslpp::interop::float3),
                        .NormalStride      = sizeof(hlslpp::interop::float3),
                        .TangentStride     = sizeof(hlslpp::interop::float4),
                        .TexCoordStride    = sizeof(hlslpp::interop::float2),
                        .IndexStride       = sizeof(Uint32),
                        .VertexCount       = SubMesh.VertexCount,
                        .IndexCount        = static_cast<Uint32>(SubMesh.Indices.size()),
                        .MaterialIndex     = m_MaterialResolver.Resolve(Material, HasReadyUV0, HasReadyTangents),
                    });
                }
                if (MeshGeometries.empty())
                    break;
            }
            if (MeshGeometries.empty())
                continue;
            const auto InstanceIndex = static_cast<Uint32>(Instances.size());
            const auto FirstGeometry = static_cast<Uint32>(GeometrySources.size());
            GeometrySources.insert(GeometrySources.end(), MeshGeometries.begin(), MeshGeometries.end());
            GeometryInstances.push_back(RHIRayTracingInstanceData{
                .FirstGeometry = FirstGeometry,
                .GeometryCount = static_cast<Uint32>(MeshGeometries.size()),
            });

            Instances.push_back(RHIAccelerationStructureInstance{
                .BottomLevelRef = std::move(BlasPayloadRef),
                .Transform      = ToAccelerationStructureInstanceTransform(Renderable.WorldTransform),
                .CustomIndex    = InstanceIndex,
            });
        }
        if (Instances.empty())
            return Result;

        const auto& View          = Scene.Views.front();
        auto        ViewOutputRef = View.ColorRT;
        auto*       ViewOutput    = ViewOutputRef.TryGet();
        if (!ViewOutput)
            return Result;
        const bool bTargetsChanged = EnsureOutputTargets(ViewOutput->GetWidth(), ViewOutput->GetHeight());
        auto       OutputRef       = m_Output;
        auto       AccumulationRef = m_Accumulation;
        auto*      Output          = OutputRef.TryGet();
        auto*      Accumulation    = AccumulationRef.TryGet();
        if (!Output || !Accumulation)
            return Result;

        const auto SceneSignature = BuildSceneSignature(
            Scene, View, Instances.size(), GeometrySources.size(), m_MaterialResolver.GetMaterials());
        if (bTargetsChanged || !m_HasAccumulation || SceneSignature != m_LastSceneSignature) {
            m_SampleIndex        = 0;
            m_LastSceneSignature = SceneSignature;
            m_HasAccumulation    = true;
        }

        if (m_Parameters.GetLayoutId() != Pipeline->GetShaderParameterLayout().GetId())
            m_Parameters = RHIShaderParameters::Create(*Pipeline);
        const auto Textures = m_MaterialResolver.BuildTextureArray();
        if (auto R = m_Parameters.SetResourceArray("g_textures.uTextures", Textures); !R)
            return std::unexpected(R.error().Append("RayTracingRenderer texture parameter binding failed"));
        if (auto R = m_Parameters.SetSampler("g_samplers.uSamplerLinear", SamplerLinear); !R)
            return std::unexpected(R.error().Append("RayTracingRenderer linear sampler parameter binding failed"));
        if (auto R = m_Parameters.SetSampler("g_samplers.uSamplerAniso", SamplerAniso); !R)
            return std::unexpected(R.error().Append("RayTracingRenderer anisotropic sampler parameter binding failed"));
        if (auto R = m_Parameters.SetTopLevelAccelerationStructure("g_rayTracing.tlas", TlasPayload); !R)
            return std::unexpected(R.error().Append("RayTracingRenderer TLAS parameter binding failed"));
        if (auto R = m_Parameters.SetStorageRenderTarget("g_rayTracing.output", Output); !R)
            return std::unexpected(R.error().Append("RayTracingRenderer output parameter binding failed"));
        if (auto R = m_Parameters.SetStorageRenderTarget("g_rayTracing.accumulation", Accumulation); !R)
            return std::unexpected(R.error().Append("RayTracingRenderer accumulation parameter binding failed"));
        const auto GeometryInstanceDataBytes = std::as_bytes(std::span{GeometryInstances});
        auto       GeometryInstanceBuffer =
            RHIRenderDevice::Get().AllocateTransientShaderStorageBuffer(GeometryInstanceDataBytes.size_bytes());
        if (!GeometryInstanceBuffer)
            return std::unexpected(GeometryInstanceBuffer.error().Append(
                "RayTracingRenderer geometry-instance transient storage allocation failed"));
        if (auto R = m_Parameters.SetTransientShaderStorageBuffer("g_rayTracing.instances", *GeometryInstanceBuffer);
            !R)
            return std::unexpected(R.error().Append("RayTracingRenderer geometry-instance parameter binding failed"));

        auto GeometryBuffer = RHIRenderDevice::Get().AllocateTransientShaderStorageBuffer(
            GeometrySources.size() * sizeof(RHIRayTracingGeometryData));
        if (!GeometryBuffer)
            return std::unexpected(
                GeometryBuffer.error().Append("RayTracingRenderer geometry transient storage allocation failed"));
        if (auto R = m_Parameters.SetTransientShaderStorageBuffer("g_rayTracing.geometries", *GeometryBuffer); !R)
            return std::unexpected(R.error().Append("RayTracingRenderer geometry parameter binding failed"));

        const auto MaterialDataBytes = std::as_bytes(m_MaterialResolver.GetMaterials());
        auto       MaterialBuffer =
            RHIRenderDevice::Get().AllocateTransientShaderStorageBuffer(MaterialDataBytes.size_bytes());
        if (!MaterialBuffer)
            return std::unexpected(
                MaterialBuffer.error().Append("RayTracingRenderer material transient storage allocation failed"));
        if (auto R = m_Parameters.SetTransientShaderStorageBuffer("g_rayTracing.materials", *MaterialBuffer); !R) {
            return std::unexpected(R.error().Append("RayTracingRenderer material parameter binding failed"));
        }
        const auto ViewData   = BuildViewConstants(View, m_SampleIndex);
        auto       ViewBuffer = RHIRenderDevice::Get().AllocateTransientConstantBuffer(sizeof(ViewData));
        if (!ViewBuffer)
            return std::unexpected(
                ViewBuffer.error().Append("RayTracingRenderer view transient constant allocation failed"));
        if (auto R = m_Parameters.SetTransientConstantBuffer("g_rayTracing.view", *ViewBuffer); !R)
            return std::unexpected(R.error().Append("RayTracingRenderer view parameter binding failed"));

        RHINonRenderingPass Pass = {};
        if (auto R = Pass.WriteTransientShaderStorageBuffer(*MaterialBuffer, MaterialDataBytes); !R)
            return std::unexpected(R.error().Append("RayTracingRenderer material transient storage write failed"));
        if (auto R = Pass.WriteTransientConstantBuffer(*ViewBuffer, std::as_bytes(std::span{&ViewData, 1})); !R)
            return std::unexpected(R.error().Append("RayTracingRenderer view transient constant write failed"));
        Pass.WriteRayTracingGeometryData(
            *GeometryInstanceBuffer, *GeometryBuffer, std::move(GeometryInstances), std::move(GeometrySources));
        Pass.BuildOrUpdateTopLevelAccelerationStructure(TlasRef, Instances);
        Pass.SetRayTracingPipeline(PipelineRef);
        Pass.BindShaderParameters(
            PipelineRef,
            m_Parameters,
            RHIShaderParameterResources{
                .SampledTextures = std::vector<RHIRef<RHISampledTexture>>{m_MaterialResolver.GetTextureRefs().begin(),
                                                                          m_MaterialResolver.GetTextureRefs().end()},
                .Samplers        = {SamplerLinearRef, SamplerAnisoRef},
                .RenderTargets   = {OutputRef, AccumulationRef},
            });
        Pass.TraceRays(PipelineRef, Output->GetWidth(), Output->GetHeight());
        Result.CmdList.Scopes.push_back(std::move(Pass));
        Result.CmdList.PresentSourceRef = OutputRef;
        ++m_SampleIndex;
        return Result;
    }

  private:
    [[nodiscard]] static auto RequestSampler(const RHISamplerDesc& Desc) -> RHIRef<RHISampler> {
        auto Sampler = RHIRenderDevice::Get().CreateSampler(Desc);
        if (!Sampler) {
            LogError("Failed to queue ray-tracing renderer sampler creation: {}", Sampler.error().ToString());
            return {};
        }
        return std::move(*Sampler);
    }

    auto EnsureOutputTargets(Uint32 Width, Uint32 Height) -> bool {
        const auto OutputKey       = Format("ray_tracing_output_{}x{}", Width, Height);
        const auto AccumulationKey = Format("ray_tracing_accumulation_{}x{}", Width, Height);
        if (m_OutputKey != OutputKey || m_AccumulationKey != AccumulationKey) {
            m_Output          = {};
            m_Accumulation    = {};
            m_OutputKey       = OutputKey;
            m_AccumulationKey = AccumulationKey;
        }

        if (!m_Output) {
            auto Output = RHIRenderDevice::Get().CreateRenderTarget(RHIRenderTargetDesc{
                .Width  = Width,
                .Height = Height,
                .Format = RHIFormat::B8G8R8A8_UNORM,
                .Usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderStorage | RHITextureUsage::FrameOutput,
            });
            if (!Output) {
                LogError("Failed to queue ray-tracing output target creation: {}", Output.error().ToString());
                return false;
            }
            m_Output = std::move(*Output);
            return true;
        }
        if (!m_Output.TryGet())
            return true;
        if (!m_Accumulation) {
            auto Accumulation = RHIRenderDevice::Get().CreateRenderTarget(RHIRenderTargetDesc{
                .Width  = Width,
                .Height = Height,
                .Format = RHIFormat::R32G32B32A32_SFLOAT,
                .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderStorage,
            });
            if (!Accumulation) {
                LogError("Failed to queue ray-tracing accumulation target creation: {}", Accumulation.error().ToString());
                return false;
            }
            m_Accumulation = std::move(*Accumulation);
            return true;
        }
        return false;
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

    [[nodiscard]] static auto BuildSceneSignature(const SceneSnapshot&                Scene,
                                                  const RenderViewSnapshot&           View,
                                                  std::size_t                         InstanceCount,
                                                  std::size_t                         GeometryCount,
                                                  std::span<const PbrMaterialGpuData> MaterialData) -> Uint64 {
        Uint64 Signature = HashMatrix(1469598103934665603ULL, View.ViewProjection);
        Signature        = HashCombine(Signature, InstanceCount);
        Signature        = HashCombine(Signature, GeometryCount);
        for (const auto& Renderable : Scene.Renderables) {
            Signature = HashCombine(Signature, std::hash<String>{}(Renderable.MeshAsset));
            Signature = HashMatrix(Signature, Renderable.WorldTransform);
            Signature = HashCombine(Signature, HashFloat(Renderable.Material.BaseColor.x));
            Signature = HashCombine(Signature, HashFloat(Renderable.Material.BaseColor.y));
            Signature = HashCombine(Signature, HashFloat(Renderable.Material.BaseColor.z));
            Signature = HashCombine(Signature, HashFloat(Renderable.Material.Metallic));
            Signature = HashCombine(Signature, HashFloat(Renderable.Material.Roughness));
        }
        for (const auto& Material : MaterialData) {
            Signature = HashCombine(Signature, HashFloat(Material.BaseColorFactor.x));
            Signature = HashCombine(Signature, HashFloat(Material.BaseColorFactor.y));
            Signature = HashCombine(Signature, HashFloat(Material.BaseColorFactor.z));
            Signature = HashCombine(Signature, HashFloat(Material.BaseColorFactor.w));
            Signature = HashCombine(Signature, HashFloat(Material.RoughnessFactor));
            Signature = HashCombine(Signature, HashFloat(static_cast<float>(Material.BaseColorTextureIndex)));
        }
        return Signature;
    }

    [[nodiscard]] static auto BuildViewConstants(const RenderViewSnapshot& View, Uint32 SampleIndex)
        -> RayTracingViewConstants {
        return RayTracingViewConstants{
            .ViewProjectionInverse = hlslpp::inverse(View.ViewProjection),
            .CameraPosition        = hlslpp::interop::float4{hlslpp::float4{
                View.CameraPosition.x, View.CameraPosition.y, View.CameraPosition.z, 1.0f}},
            .PathSettings          = hlslpp::interop::float4{hlslpp::float4{
                static_cast<float>(SampleIndex), static_cast<float>(kPathTracingMaxBounces), 0.0f, 0.0f}},
        };
    }

    RHIRef<RHIRayTracingPipeline>                      m_Pipeline           = nullptr;
    ResourceRef<ResourceTopLevelAccelerationStructure> m_Tlas               = {};
    RHIRef<RHISampler>                                 m_SamplerLinear      = nullptr;
    RHIRef<RHISampler>                                 m_SamplerAniso       = nullptr;
    PbrMaterialResolver                                m_MaterialResolver   = {};
    RHIRef<RHIRenderTarget>                            m_Output             = nullptr;
    RHIRef<RHIRenderTarget>                            m_Accumulation       = nullptr;
    String                                             m_OutputKey          = {};
    String                                             m_AccumulationKey    = {};
    RHIShaderParameters                                m_Parameters         = {};
    Uint64                                             m_LastSceneSignature = 0;
    Uint32                                             m_SampleIndex        = 0;
    bool                                               m_HasAccumulation    = false;
};

RendererFactory::AutoRegistrar<RayTracingRenderer> RegRayTracingRenderer{"RayTracing"};

} // namespace SoulEngine
