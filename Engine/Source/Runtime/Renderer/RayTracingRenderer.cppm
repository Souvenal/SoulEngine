module;

// needed for offsetof
#include <cstddef>
#include <entt/entity/entity.hpp>
#include <hlsl++.h>

export module Renderer:RayTracingRenderer;

import Core;
import EditorTypes;
import Material;
import Resource;
import RHI;
import Scene;
import TaskGraph;

import :IRenderer;
import :Common;
import :RayTracing;

export import std;

export namespace SoulEngine {

constexpr Uint32 kPathTracingMaxBounces = 6;

/// @brief Ray-tracing frame ABI extending shared frame data with path settings.
struct alignas(16) RayTracingFrameConstants {
    RendererFrameConstants              Common = {};
    alignas(16) hlslpp::interop::float4 PathSettings = hlslpp::interop::float4{
        hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f}};
};
static_assert(sizeof(RayTracingFrameConstants) == 32);
static_assert(offsetof(RayTracingFrameConstants, Common) == 0);
static_assert(offsetof(RayTracingFrameConstants, PathSettings) == 16);

struct alignas(16) RayTracingLightGpuData {
    alignas(16) hlslpp::interop::float4 ColorIntensity = hlslpp::interop::float4{
        hlslpp::float4{1.0f, 1.0f, 1.0f, 0.0f}};
    alignas(16) hlslpp::interop::float4 PositionRange = hlslpp::interop::float4{hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f}};
    alignas(16) hlslpp::interop::float4 DirectionType = hlslpp::interop::float4{
        hlslpp::float4{0.0f, 0.0f, -1.0f, 0.0f}};
    alignas(16) hlslpp::interop::float4 SpotCone = hlslpp::interop::float4{hlslpp::float4{1.0f, 1.0f, 0.0f, 0.0f}};
};
static_assert(sizeof(RayTracingLightGpuData) == 64,
              "RayTracingLightGpuData must match RayTracing.slang storage-buffer layout");

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
        const auto ShaderPath      = ConfigManager::Get().EngineShadersDirPath() / "RayTracing.slang";
        auto&      Resources       = ResourceManager::Get();
        auto       PipelineRequest = RequestRayTracingPipeline(
            "RayTracingPass",
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
            });
        m_Tlas             = Resources.RequestTopLevelAccelerationStructureRef("ray_tracing_renderer_main",
                                                                               {.InitialInstanceCapacity = 16});
        auto SamplerLinear = RHIRenderDevice::Get().CreateSampler("Renderer/RayTracing/Sampler/Linear",
                                                                  {.Profile = RHISamplerProfile::LinearRepeat});
        if (!SamplerLinear)
            return std::unexpected(SamplerLinear.error().Append("Ray-tracing linear sampler creation failed"));
        m_SamplerLinear = std::move(*SamplerLinear);

        auto SamplerAniso = RHIRenderDevice::Get().CreateSampler("Renderer/RayTracing/Sampler/Anisotropic",
                                                                 {.Profile = RHISamplerProfile::AnisotropicRepeat});
        if (!SamplerAniso)
            return std::unexpected(SamplerAniso.error().Append("Ray-tracing anisotropic sampler creation failed"));
        m_SamplerAniso = std::move(*SamplerAniso);

        if (!PipelineRequest)
            return std::unexpected(PipelineRequest.error().Append("RayTracingRenderer pipeline request failed"));
        m_Pipeline = std::move(*PipelineRequest);
        return {};
    }

    auto OnDetach() -> void override {
        m_Pipeline           = {};
        m_Tlas               = {};
        m_SamplerLinear      = {};
        m_SamplerAniso       = {};
        m_Output             = {};
        m_Accumulation       = {};
        m_OutputKey          = {};
        m_AccumulationKey    = {};
        m_LastSceneSignature = 0;
        m_SampleIndex        = 0;
        m_HasAccumulation    = false;
    }

    [[nodiscard]] auto Render(const GameSnapshot& Scene, const EditorSnapshot& Editor)
        -> std::expected<RenderResult, ErrorMessage> override {
        RenderResult Result = {};
        std::vector<CameraViewRecord> Views = Scene.Views;
        Views.insert(Views.end(), Editor.Views.begin(), Editor.Views.end());
        if (Views.empty())
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
        Instances.reserve(Scene.Instances.size());
        std::vector<RHIRayTracingInstanceData> GeometryInstances = {};
        GeometryInstances.reserve(Scene.Instances.size());
        std::vector<RHIRayTracingGeometryDesc> GeometrySources = {};
        std::vector<MaterialRecord::GpuData> MaterialRecords = {MaterialRecord::GpuData{}};
        std::unordered_map<const MaterialRecord*, Uint32> MaterialIDs = {};
        MaterialIDs.reserve(Scene.Instances.size());
        for (const auto& Renderable : Scene.Instances) {
            if (!Renderable.Geometry)
                continue;

            const auto& Record = *Renderable.Geometry;
            if (Record.CacheKey.empty())
                continue;

            // Resource only needs the position/index RHI inputs for BLAS creation;
            // Scene-owned GeometryRecord stays at the Scene/Renderer boundary.
            const std::vector<RHITriangleAccelerationStructureGeometryDesc> BlasGeometries{
                RHITriangleAccelerationStructureGeometryDesc{
                    .VertexBufferRef = Record.PositionBuffer,
                    .IndexBufferRef  = Record.IndexBuffer,
                }};

            auto  BlasRef = Resources.RequestBottomLevelAccelerationStructureRef(Record.CacheKey, BlasGeometries);
            auto* Blas    = Resources.TryGetReady(BlasRef);
            auto  BlasPayloadRef =
                Blas ? Blas->GetRhiPayloadRef() : RHIRef<RHIBottomLevelAccelerationStructure>{nullptr};
            auto* BlasPayload = BlasPayloadRef.TryGet();
            if (!Blas || !BlasPayload)
                continue;

            auto       PositionRef      = Record.PositionBuffer;
            auto       NormalRef        = Record.NormalBuffer;
            auto       TangentRef       = Record.TangentBuffer;
            auto       TexCoordRef      = Record.TexCoordBuffer;
            auto       IndexRef         = Record.IndexBuffer;
            auto*      Position         = PositionRef.TryGet();
            auto*      Normal           = NormalRef.TryGet();
            auto*      Tangent          = TangentRef.TryGet();
            auto*      TexCoord         = TexCoordRef.TryGet();
            auto*      Index            = IndexRef.TryGet();
            if (!Tangent) {
                TangentRef = PositionRef;
                Tangent    = Position;
            }
            if (!TexCoord) {
                TexCoordRef = PositionRef;
                TexCoord    = Position;
            }
            if (!Position || !Normal || !Tangent || !TexCoord || !Index || Record.IndexCount == 0)
                continue;

            Uint32 MaterialIndex = 0;
            if (Renderable.Material) {
                const auto* Material = std::addressof(*Renderable.Material);
                const auto [MaterialIt, Inserted] =
                    MaterialIDs.emplace(Material, static_cast<Uint32>(MaterialRecords.size()));
                MaterialIndex = MaterialIt->second;
                if (Inserted)
                    MaterialRecords.emplace_back(Material->BuildGpuData(Scene.Textures));
            }

            const auto FirstGeometry = static_cast<Uint32>(GeometrySources.size());
            GeometrySources.push_back(RHIRayTracingGeometryDesc{
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
                .VertexCount       = Record.IndexCount,
                .IndexCount        = Record.IndexCount,
                .MaterialIndex     = MaterialIndex,
            });
            GeometryInstances.push_back(RHIRayTracingInstanceData{
                .FirstGeometry = FirstGeometry,
                .GeometryCount = 1,
                .EntityId      = Renderable.EntityId,
            });

            Instances.push_back(RHIAccelerationStructureInstance{
                .BottomLevelRef = std::move(BlasPayloadRef),
                .Transform      = ToAccelerationStructureInstanceTransform(Renderable.WorldTransform),
                .CustomIndex    = Renderable.EntityId,
            });
        }
        if (Instances.empty())
            return Result;

        const auto& View            = Views.front();
        auto        ViewOutputRef   = View.Targets.GBuffer.AlbedoRT;
        auto        ViewNormalRef   = View.Targets.GBuffer.NormalRT;
        auto        ViewEntityIdRef = View.Targets.GBuffer.EntityIdRT;
        auto*       ViewOutput      = ViewOutputRef.TryGet();
        auto*       ViewNormal      = ViewNormalRef.TryGet();
        auto*       ViewEntityId    = ViewEntityIdRef.TryGet();
        if (!ViewOutput || !ViewNormal || !ViewEntityId)
            return Result;
        const bool bTargetsChanged = EnsureOutputTargets(ViewOutput->GetWidth(), ViewOutput->GetHeight());
        auto       OutputRef       = m_Output;
        auto       AccumulationRef = m_Accumulation;
        auto*      Output          = OutputRef.TryGet();
        auto*      Accumulation    = AccumulationRef.TryGet();
        if (!Output || !Accumulation)
            return Result;

        const auto SceneSignature = BuildSceneSignature(
            Scene, View, Instances.size(), GeometrySources.size(), MaterialRecords);
        if (bTargetsChanged || !m_HasAccumulation || SceneSignature != m_LastSceneSignature) {
            m_SampleIndex        = 0;
            m_LastSceneSignature = SceneSignature;
            m_HasAccumulation    = true;
        }

        const auto GeometryInstanceDataBytes = std::as_bytes(std::span{GeometryInstances});
        auto GeometryInstanceBuffer =
            RHIRenderDevice::Get().CreateTransientShaderStorageBuffer(RHITransientShaderStorageBufferDesc{
                .Data = GeometryInstanceDataBytes,
            });
        if (!GeometryInstanceBuffer)
            return std::unexpected(GeometryInstanceBuffer.error().Append(
                "RayTracingRenderer geometry-instance transient storage allocation failed"));
        std::vector<RHIRayTracingGeometryData> GeometryGpuRecords = {};
        GeometryGpuRecords.reserve(GeometrySources.size());
        for (const auto& Geometry : GeometrySources)
            GeometryGpuRecords.emplace_back(Geometry.BuildGpuData());
        auto GeometryBuffer =
            RHIRenderDevice::Get().CreateTransientShaderStorageBuffer(RHITransientShaderStorageBufferDesc{
                .Data = std::as_bytes(std::span{GeometryGpuRecords}),
            });
        if (!GeometryBuffer)
            return std::unexpected(
                GeometryBuffer.error().Append("RayTracingRenderer geometry transient storage allocation failed"));
        const auto MaterialDataBytes = std::as_bytes(std::span{MaterialRecords});
        auto MaterialBuffer =
            RHIRenderDevice::Get().CreateTransientShaderStorageBuffer(RHITransientShaderStorageBufferDesc{
                .Data = MaterialDataBytes,
            });
        if (!MaterialBuffer)
            return std::unexpected(
                MaterialBuffer.error().Append("RayTracingRenderer material transient storage allocation failed"));
        const auto Lights      = BuildLightData(Scene.Lights);
        const auto LightBytes  = std::as_bytes(std::span{Lights});
        auto LightBuffer =
            RHIRenderDevice::Get().CreateTransientShaderStorageBuffer(RHITransientShaderStorageBufferDesc{
                .Data = LightBytes,
            });
        if (!LightBuffer)
            return std::unexpected(
                LightBuffer.error().Append("RayTracingRenderer light transient storage allocation failed"));
        const auto FrameData = RayTracingFrameConstants{
            .Common = RendererFrameConstants{
                .Time          = Scene.Time,
                .ExposureEV100 = View.ExposureEV100,
                .LightCount    = static_cast<Uint32>(Scene.Lights.size()),
            },
            .PathSettings = hlslpp::interop::float4{hlslpp::float4{
                static_cast<Float32>(m_SampleIndex), static_cast<Float32>(kPathTracingMaxBounces), 0.0f, 0.0f}},
        };
        const auto ViewData = RendererViewConstants{View};
        auto FrameBuffer = RHIRenderDevice::Get().CreateTransientConstantBuffer(RHITransientConstantBufferDesc{
            .Data = std::as_bytes(std::span{&FrameData, 1}),
        });
        if (!FrameBuffer)
            return std::unexpected(
                FrameBuffer.error().Append("RayTracingRenderer frame transient constant allocation failed"));
        auto ViewBuffer = RHIRenderDevice::Get().CreateTransientConstantBuffer(RHITransientConstantBufferDesc{
            .Data = std::as_bytes(std::span{&ViewData, 1}),
        });
        if (!ViewBuffer)
            return std::unexpected(
                ViewBuffer.error().Append("RayTracingRenderer view transient constant allocation failed"));
        auto Pass = std::make_unique<PathTracingPass>(PipelineRef);
        Pass->SetInput(PathTracingPassInput{
            .Tlas = TlasRef,
            .Output = OutputRef,
            .Accumulation = AccumulationRef,
            .PrimaryNormal = ViewNormalRef,
            .PrimaryEntityId = ViewEntityIdRef,
            .LinearSampler = SamplerLinearRef,
            .AnisotropicSampler = SamplerAnisoRef,
            .Textures = Scene.Textures,
            .InstancesBuffer = *GeometryInstanceBuffer,
            .GeometryBuffer = *GeometryBuffer,
            .MaterialBuffer = *MaterialBuffer,
            .LightBuffer = *LightBuffer,
            .FrameBuffer = *FrameBuffer,
            .ViewBuffer = *ViewBuffer,
            .AccelerationInstances = std::vector<RHIAccelerationStructureInstance>{
                Instances.begin(), Instances.end()},
            .Width = Output->GetWidth(),
            .Height = Output->GetHeight(),
        });
        Result.CmdList.Passes.push_back(std::move(Pass));
        ++m_SampleIndex;
        return Result;
    }

  private:
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
            auto Output = RHIRenderDevice::Get().CreateRenderTarget("Renderer/RayTracing/Output",
                                                                    RHIRenderTargetDesc{
                                                                        .Width  = Width,
                                                                        .Height = Height,
                                                                        .Format = RHIFormat::B8G8R8A8_UNORM,
                                                                        .Usage  = RHITextureUsage::RenderTarget |
                                                                                  RHITextureUsage::ShaderStorage |
                                                                                  RHITextureUsage::FrameOutput,
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
            auto Accumulation = RHIRenderDevice::Get().CreateRenderTarget(
                "Renderer/RayTracing/Accumulation",
                RHIRenderTargetDesc{
                    .Width  = Width,
                    .Height = Height,
                    .Format = RHIFormat::R32G32B32A32_SFLOAT,
                    .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderStorage,
                });
            if (!Accumulation) {
                LogError("Failed to queue ray-tracing accumulation target creation: {}",
                         Accumulation.error().ToString());
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

    [[nodiscard]] static auto BuildSceneSignature(const GameSnapshot&             Scene,
                                                  const CameraViewRecord&         View,
                                                  std::size_t                     InstanceCount,
                                                  std::size_t                     GeometryCount,
                                                  std::span<const MaterialRecord::GpuData> MaterialData) -> Uint64 {
        Uint64 Signature = HashMatrix(1469598103934665603ULL, View.ViewProjection);
        Signature        = HashCombine(Signature, InstanceCount);
        Signature        = HashCombine(Signature, GeometryCount);
        for (const auto& Renderable : Scene.Instances) {
            if (Renderable.Geometry)
                Signature = HashCombine(Signature, std::hash<String>{}(Renderable.Geometry->CacheKey));
            Signature = HashMatrix(Signature, Renderable.WorldTransform);
            Signature = HashCombine(
                Signature,
                Renderable.Material ? std::hash<const MaterialRecord*>{}(std::addressof(*Renderable.Material)) : 0);
            Signature = HashCombine(Signature, Renderable.EntityId);
        }
        Signature = HashCombine(Signature, HashFloat(View.ExposureEV100));
        for (const auto& Light : Scene.Lights) {
            Signature = HashCombine(Signature, static_cast<Uint64>(Light.Type));
            Signature = HashCombine(Signature, HashFloat(Light.Color.x));
            Signature = HashCombine(Signature, HashFloat(Light.Color.y));
            Signature = HashCombine(Signature, HashFloat(Light.Color.z));
            Signature = HashCombine(Signature, HashFloat(Light.Intensity));
            Signature = HashCombine(Signature, HashFloat(Light.Position.x));
            Signature = HashCombine(Signature, HashFloat(Light.Position.y));
            Signature = HashCombine(Signature, HashFloat(Light.Position.z));
            Signature = HashCombine(Signature, HashFloat(Light.RangeMeters));
            Signature = HashCombine(Signature, HashFloat(Light.Direction.x));
            Signature = HashCombine(Signature, HashFloat(Light.Direction.y));
            Signature = HashCombine(Signature, HashFloat(Light.Direction.z));
            Signature = HashCombine(Signature, HashFloat(Light.InnerConeCosine));
            Signature = HashCombine(Signature, HashFloat(Light.OuterConeCosine));
        }
        for (const auto& Material : MaterialData) {
            Signature = HashCombine(Signature, HashFloat(Material.BaseColorFactor.x));
            Signature = HashCombine(Signature, HashFloat(Material.BaseColorFactor.y));
            Signature = HashCombine(Signature, HashFloat(Material.BaseColorFactor.z));
            Signature = HashCombine(Signature, HashFloat(Material.BaseColorFactor.w));
            Signature = HashCombine(Signature, HashFloat(Material.RoughnessFactor));
            Signature = HashCombine(Signature, Material.BaseColorTexture[0]);
        }
        return Signature;
    }

    [[nodiscard]] static auto BuildLightData(std::span<const LightRecord> Lights)
        -> std::vector<RayTracingLightGpuData> {
        std::vector<RayTracingLightGpuData> Result = {};
        Result.reserve(std::max<std::size_t>(Lights.size(), 1));
        for (const auto& Light : Lights) {
            Result.emplace_back(RayTracingLightGpuData{
                .ColorIntensity = hlslpp::interop::float4{hlslpp::float4{
                    Light.Color.x, Light.Color.y, Light.Color.z, Light.Intensity}},
                .PositionRange  = hlslpp::interop::float4{hlslpp::float4{
                    Light.Position.x, Light.Position.y, Light.Position.z, Light.RangeMeters}},
                .DirectionType  = hlslpp::interop::float4{hlslpp::float4{
                    Light.Direction.x, Light.Direction.y, Light.Direction.z, static_cast<Float32>(Light.Type)}},
                .SpotCone       = hlslpp::interop::float4{hlslpp::float4{
                    Light.InnerConeCosine, Light.OuterConeCosine, Light.CastsShadows ? 1.0f : 0.0f, 0.0f}},
            });
        }
        if (Result.empty())
            Result.emplace_back();
        return Result;
    }
    RHIRef<RHIRayTracingPipeline>                      m_Pipeline           = nullptr;
    ResourceRef<ResourceTopLevelAccelerationStructure> m_Tlas               = {};
    RHIRef<RHISampler>                                 m_SamplerLinear      = nullptr;
    RHIRef<RHISampler>                                 m_SamplerAniso       = nullptr;
    RHIRef<RHIRenderTarget>                            m_Output             = nullptr;
    RHIRef<RHIRenderTarget>                            m_Accumulation       = nullptr;
    String                                             m_OutputKey          = {};
    String                                             m_AccumulationKey    = {};
    Uint64                                             m_LastSceneSignature = 0;
    Uint32                                             m_SampleIndex        = 0;
    bool                                               m_HasAccumulation    = false;
};

RendererFactory::AutoRegistrar<RayTracingRenderer> RegRayTracingRenderer{"RayTracing"};

} // namespace SoulEngine
