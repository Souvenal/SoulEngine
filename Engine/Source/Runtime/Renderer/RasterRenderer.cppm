module;

// needed for offsetof
#include <cstddef>
#include <entt/entity/entity.hpp>
#include <hlsl++.h>

export module Renderer:RasterRenderer;

import Core;
import Material;
import Resource;
import RHI;
import Resource;
import Scene;
import TaskGraph;

import :IRenderer;
import :PostProcess.EditorPostProcess;

export import std;

export namespace SoulEngine {

/// @brief Constant buffer layout matching RasterGeometry.slang RasterFrameData.
struct alignas(16) RasterFrameConstants {
    Float32 Time          = 0.0f;
    Float32 ExposureEV100 = 15.0f;
    Uint32  LightCount    = 0;
};
static_assert(sizeof(RasterFrameConstants) == 16,
              "RasterFrameConstants must match RasterGeometry.slang RasterFrameData std140 layout");
static_assert(offsetof(RasterFrameConstants, Time) == 0, "RasterFrameConstants::Time must match RasterFrameData.time");
static_assert(offsetof(RasterFrameConstants, ExposureEV100) == 4,
              "RasterFrameConstants::ExposureEV100 must match RasterFrameData.exposureEV100");
static_assert(offsetof(RasterFrameConstants, LightCount) == 8,
              "RasterFrameConstants::LightCount must match RasterFrameData.lightCount");

struct alignas(16) DeferredFrameConstants {
    Float32 ExposureEV100 = 15.0f;
    Uint32  LightCount    = 0;
};
static_assert(sizeof(DeferredFrameConstants) == 16);

struct alignas(16) DeferredViewConstants {
    alignas(16) hlslpp::float4x4 InverseViewProjection = hlslpp::float4x4::identity();
    alignas(16) hlslpp::interop::float4 CameraPosition = hlslpp::interop::float4{
        hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f}};
    hlslpp::interop::float2 ViewportSize = hlslpp::interop::float2{hlslpp::float2{0.0f, 0.0f}};
};
static_assert(sizeof(DeferredViewConstants) == 96);

/// @brief Constant buffer layout matching RasterGeometry.slang ViewData.
struct alignas(16) RasterViewConstants {
    alignas(16) hlslpp::float4x4 ViewProjection        = hlslpp::float4x4::identity();
    alignas(16) hlslpp::interop::float4 CameraPosition = hlslpp::interop::float4{
        hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f}};
};
static_assert(sizeof(RasterViewConstants) == 80,
              "RasterViewConstants must match RasterGeometry.slang RasterViewData std140 layout");

/// @brief Storage-buffer layout matching RasterGeometry.slang LightData.
struct alignas(16) LightGpuData {
    alignas(16) hlslpp::interop::float4 ColorIntensity = hlslpp::interop::float4{
        hlslpp::float4{1.0f, 1.0f, 1.0f, 0.0f}};
    alignas(16) hlslpp::interop::float4 PositionRange = hlslpp::interop::float4{hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f}};
    alignas(16) hlslpp::interop::float4 DirectionType = hlslpp::interop::float4{
        hlslpp::float4{0.0f, 0.0f, -1.0f, 0.0f}};
    alignas(16) hlslpp::interop::float4 SpotCone = hlslpp::interop::float4{hlslpp::float4{1.0f, 1.0f, 0.0f, 0.0f}};
};
static_assert(sizeof(LightGpuData) == 64, "LightGpuData must match RasterGeometry.slang storage-buffer layout");
/// @brief Storage-buffer layout matching RasterGeometry.slang InstanceData.
struct alignas(16) InstanceGpuData {
    alignas(16) hlslpp::float4x4 WorldTransform        = hlslpp::float4x4::identity();
    alignas(16) hlslpp::interop::float4 BoundingSphere = hlslpp::interop::float4{
        hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f}};
    Uint32 MaterialIndex = 0;
    Uint32 EntityId      = 0;
    Uint32 GeometryID    = 0;
};
static_assert(sizeof(InstanceGpuData) == 96, "InstanceGpuData must match RasterGeometry.slang storage-buffer layout");
static_assert(offsetof(InstanceGpuData, WorldTransform) == 0);
static_assert(offsetof(InstanceGpuData, BoundingSphere) == 64);
static_assert(offsetof(InstanceGpuData, MaterialIndex) == 80);
static_assert(offsetof(InstanceGpuData, EntityId) == 84);
static_assert(offsetof(InstanceGpuData, GeometryID) == 88);

struct RasterInstanceIndex {
    Uint32 Value = 0;
};

struct RasterIndirectCommand {
    Uint32 VertexCount   = 0;
    Uint32 InstanceCount = 1;
    Uint32 FirstVertex   = 0;
    Uint32 FirstInstance = 0;
};
static_assert(sizeof(RasterIndirectCommand) == sizeof(Uint32) * 4);

struct RasterViewParameterState {
    RHIRenderTarget*    ViewRenderTargetPtr = nullptr;
    RHIShaderParameters Parameters          = {};
};

/// @brief One concrete indexed draw consumed by the forward raster pass.
struct InstanceData {
    Uint32                       EntityId       = 0;
    RHIRef<RHIVertexBuffer>      PositionVB     = nullptr;
    RHIRef<RHIVertexBuffer>      NormalVB       = nullptr;
    RHIRef<RHIVertexBuffer>      TangentVB      = nullptr;
    RHIRef<RHIVertexBuffer>      UVVB           = nullptr;
    RHIRef<RHIIndexBuffer>       IndexBuffer    = nullptr;
    Uint32                       MaterialID     = 0;
    bool                         HasUV0             = false;
    bool                         HasTangents        = false;
    hlslpp::float4x4             WorldTransform = hlslpp::float4x4::identity();
    hlslpp::interop::float4      BoundingSphere = hlslpp::interop::float4{hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f}};
};

struct RasterMeshCacheEntry {
    String                    Asset = {};
    ResourceRef<ResourceMesh> Mesh  = {};
};

/// @brief Single-material metallic-roughness forward renderer.
class RasterRenderer final : public IRenderer {
  public:
    RasterRenderer() = default;
    ~RasterRenderer() override {
        OnDetach();
    }

    [[nodiscard]] auto OnAttach() -> std::expected<void, ErrorMessage> override {
        const auto ShaderPath = ConfigManager::Get().EngineShadersDirPath() / "RasterGeometry.slang";

        auto PipelineRequest = RequestGraphicsPipeline(GraphicsPipelineRequest{
            .VertEntry =
                {
                    .SourcePath = ShaderPath,
                    .EntryPoint = "vertMain",
                },
            .FragEntry =
                {
                    .SourcePath = ShaderPath,
                    .EntryPoint = "fragMain",
                },
            .ColorFormats      = std::vector<RHIFormat>(GBuffer::ColorFormats.begin(), GBuffer::ColorFormats.end()),
            .DepthFormat       = GBuffer::DepthFormat,
        });
        if (!PipelineRequest)
            return std::unexpected(PipelineRequest.error().Append("Raster geometry graphics pipeline request failed"));
        m_Pipeline = std::move(*PipelineRequest);

        const auto DeferredShaderPath      = ConfigManager::Get().EngineShadersDirPath() / "DeferredLighting.slang";
        auto       DeferredPipelineRequest = RequestGraphicsPipeline(GraphicsPipelineRequest{
            .VertEntry    = {.SourcePath = DeferredShaderPath, .EntryPoint = "vertMain"},
            .FragEntry    = {.SourcePath = DeferredShaderPath, .EntryPoint = "fragMain"},
            .ColorFormats = {RHIFormat::B8G8R8A8_UNORM},
        });
        if (!DeferredPipelineRequest)
            return std::unexpected(DeferredPipelineRequest.error().Append("Deferred lighting pipeline request failed"));
        m_DeferredPipeline = std::move(*DeferredPipelineRequest);

        auto EditorSelectionPipelineRequest = RequestEditorSelectionPipeline();
        if (!EditorSelectionPipelineRequest)
            return std::unexpected(EditorSelectionPipelineRequest.error().Append(
                "Editor selection post-process pipeline request failed"));
        m_EditorSelectionPipeline = std::move(*EditorSelectionPipelineRequest);

        m_SamplerLinear = RequestSampler({.Profile = RHISamplerProfile::LinearRepeat});
        m_SamplerAniso = RequestSampler({.Profile = RHISamplerProfile::AnisotropicRepeat});

        return {};
    }

    auto OnDetach() -> void override {
        m_Pipeline                = {};
        m_EditorSelectionPipeline = {};
        m_DeferredPipeline         = {};
        m_SamplerLinear            = {};
        m_SamplerAniso             = {};
        m_MeshCache.clear();
        m_ViewParameters.clear();
    }

    [[nodiscard]] auto Render(const SceneSnapshot& Scene) -> std::expected<RenderResult, ErrorMessage> override {
        RenderResult Result = {};
        if (Scene.Views.empty())
            return Result;

        const auto DrawInstances = BuildDrawInstances(Scene);
        for (const auto& View : Scene.Views) {
            if (auto R = RenderView(Result.CmdList, DrawInstances, View, Scene); !R)
                return std::unexpected(R.error().Append("Raster geometry view rendering failed"));
        }

        return Result;
    }

  private:
    [[nodiscard]] static auto RequestSampler(const RHISamplerDesc& Desc) -> RHIRef<RHISampler> {
        auto Sampler = RHIRenderDevice::Get().CreateSampler(Desc);
        if (!Sampler) {
            LogError("Failed to queue forward renderer sampler creation: {}", Sampler.error().ToString());
            return {};
        }
        return std::move(*Sampler);
    }

    [[nodiscard]] auto RenderView(RHICommandList&                     CmdList,
                                  std::span<const InstanceData> DrawInstances,
                                  const RenderViewSnapshot&           View,
                                  const SceneSnapshot&                Scene) -> std::expected<void, ErrorMessage> {
        auto& Resources = ResourceManager::Get();

        const auto& AlbedoRTRef          = View.Targets.GBuffer.AlbedoRT;
        const auto& NormalRTRef          = View.Targets.GBuffer.NormalRT;
        const auto& EntityIdRTRef        = View.Targets.GBuffer.EntityIdRT;
        const auto& MaterialIdRTRef      = View.Targets.GBuffer.MaterialIdRT;
        const auto& DepthRTRef           = View.Targets.GBuffer.DepthRT;
        const auto& SceneColorRTRef      = View.Targets.SceneColorRT;
        const auto& PipelineRef                = m_Pipeline;
        const auto& EditorSelectionPipelineRef = m_EditorSelectionPipeline;
        const auto& SamplerLinearRef           = m_SamplerLinear;
        const auto& SamplerAnisoRef            = m_SamplerAniso;
        const auto& DeferredPipelineRef        = m_DeferredPipeline;
        if (!AlbedoRTRef || !NormalRTRef || !EntityIdRTRef || !MaterialIdRTRef || !DepthRTRef || !SceneColorRTRef ||
            !PipelineRef || !EditorSelectionPipelineRef || !SamplerLinearRef || !SamplerAnisoRef || !DeferredPipelineRef)
            return {};

        const auto FrameData =
            BuildFrameConstants(Scene.Time, View.ExposureEV100, static_cast<Uint32>(Scene.Lights.size()));
        const auto Lights      = BuildLightData(Scene.Lights);
        const auto LightBytes  = std::as_bytes(std::span{Lights});
        auto       LightBuffer = RHIRenderDevice::Get().AllocateTransientShaderStorageBuffer(LightBytes.size_bytes());
        if (!LightBuffer)
            return std::unexpected(
                LightBuffer.error().Append("Raster geometry light transient storage allocation failed"));
        auto FrameBuffer = RHIRenderDevice::Get().AllocateTransientConstantBuffer(sizeof(FrameData));
        if (!FrameBuffer)
            return std::unexpected(
                FrameBuffer.error().Append("Raster geometry frame transient constant allocation failed"));

        const auto ViewData   = BuildViewConstants(View);
        auto       ViewBuffer = RHIRenderDevice::Get().AllocateTransientConstantBuffer(sizeof(ViewData));
        if (!ViewBuffer)
            return std::unexpected(
                ViewBuffer.error().Append("Raster geometry view transient constant allocation failed"));

        auto& Parameters          = GetViewParameters(View, *PipelineRef);
        auto  BindFrameParameters = [&](RHIShaderParameters& Target) -> std::expected<void, ErrorMessage> {
            if (auto R = Target.SetTransientShaderStorageBuffer("g_rasterFrameView.lights", *LightBuffer); !R)
                return std::unexpected(R.error().Append("Raster geometry light parameter binding failed"));
            if (auto R = Target.SetTransientConstantBuffer("g_rasterFrameView.frame", *FrameBuffer); !R)
                return std::unexpected(R.error().Append("Raster geometry frame parameter binding failed"));
            if (auto R = Target.SetTransientConstantBuffer("g_rasterFrameView.view", *ViewBuffer); !R)
                return std::unexpected(R.error().Append("Raster geometry view parameter binding failed"));
            if (auto R = Target.SetSampler("g_samplers.uSamplerLinear", SamplerLinearRef); !R)
                return std::unexpected(R.error().Append("Raster geometry sampler parameter binding failed"));
            if (auto R = Target.SetSampler("g_samplers.uSamplerAniso", SamplerAnisoRef); !R)
                return std::unexpected(R.error().Append("Raster geometry sampler parameter binding failed"));
            return {};
        };
        if (auto R = BindFrameParameters(Parameters); !R)
            return std::unexpected(R.error());
        RHIPass GeometryPass{
            .Desc =
                RHIRenderingDesc{
                    .ColorAttachments =
                        {
                            {.TextureRef = AlbedoRTRef,
                             .ClearValue = {.R = 0.025f, .G = 0.035f, .B = 0.055f, .A = 1.0f}},
                            {.TextureRef = NormalRTRef, .ClearValue = {.R = 0.0f, .G = 0.0f, .B = 1.0f, .A = 1.0f}},
                            {.TextureRef = MaterialIdRTRef, .ClearValue = {.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f}},
                            {.TextureRef = EntityIdRTRef,
                             .ClearValue = {.UseUInt = true, .UInt = {GBuffer::BackgroundEntityId, 0, 0, 0}}},
                        },
                    .DepthAttachment =
                        RHIDepthAttachmentDesc{
                            .TextureRef = DepthRTRef,
                            .ClearValue = {.Depth = 1.0f, .Stencil = 0},
                        },
                },
        };
        if (auto R = GeometryPass.WriteTransientShaderStorageBuffer(*LightBuffer, LightBytes); !R)
            return std::unexpected(R.error().Append("Raster geometry light transient storage write failed"));
        if (auto R = GeometryPass.WriteTransientConstantBuffer(*FrameBuffer, std::as_bytes(std::span{&FrameData, 1})); !R)
            return std::unexpected(R.error().Append("Raster geometry frame transient constant write failed"));
        if (auto R = GeometryPass.WriteTransientConstantBuffer(*ViewBuffer, std::as_bytes(std::span{&ViewData, 1})); !R)
            return std::unexpected(R.error().Append("Raster geometry view transient constant write failed"));
        if (CmdList.PresentSourceRef.GetState() == RHIRefState::Unknown)
            CmdList.PresentSourceRef = SceneColorRTRef;

        std::vector<InstanceGpuData>               Instances       = {};
        std::vector<RHIRasterGeometrySource>    GeometrySources = {};
        Instances.reserve(DrawInstances.size());
        GeometrySources.reserve(DrawInstances.size());

        for (const auto& Instance : DrawInstances) {
            if (!Instance.PositionVB || !Instance.NormalVB || !Instance.TangentVB || !Instance.UVVB ||
                !Instance.IndexBuffer)
                continue;

            const auto GeometryID = static_cast<Uint32>(Instances.size());
            Instances.emplace_back(BuildInstanceData(Instance, Instance.MaterialID, GeometryID));
            GeometrySources.emplace_back(RHIRasterGeometrySource{
                .PositionBufferRef = Instance.PositionVB,
                .NormalBufferRef   = Instance.NormalVB,
                .TangentBufferRef  = Instance.TangentVB,
                .TexCoordBufferRef = Instance.UVVB,
                .IndexBufferRef    = Instance.IndexBuffer,
                .IndexCount        = static_cast<Uint32>(Instance.IndexBuffer->GetIndexCount()),
                .MaterialID        = Instance.MaterialID,
            });
        }

        auto& MaterialMgr = MaterialManager::Get();
        const auto& Textures = MaterialMgr.GetMaterialTextures();
        if (auto R = Parameters.SetResourceArray("g_textures.uTextures", Textures); !R)
            return std::unexpected(R.error().Append("Raster geometry texture parameter binding failed"));
        GeometryPass.SetFullViewport();
        GeometryPass.SetFullScissorRect();
        GeometryPass.SetGraphicsPipeline(PipelineRef);
        if (!Instances.empty()) {
            const auto InstanceDataBytes = std::as_bytes(std::span{Instances});
            auto       InstanceDataBuffer =
                RHIRenderDevice::Get().AllocateTransientShaderStorageBuffer(InstanceDataBytes.size_bytes());
            if (!InstanceDataBuffer)
                return std::unexpected(InstanceDataBuffer.error().Append(
                    "Raster geometry instance-data transient buffer allocation failed"));
            if (auto R = GeometryPass.WriteTransientShaderStorageBuffer(*InstanceDataBuffer, InstanceDataBytes); !R)
                return std::unexpected(R.error().Append("Raster geometry instance-data transient buffer write failed"));
            if (auto R = Parameters.SetTransientShaderStorageBuffer("g_rasterDraw.instances", *InstanceDataBuffer); !R)
                return std::unexpected(R.error().Append("Raster geometry instance-data storage buffer binding failed"));

            auto GeometryDataBuffer =
                RHIRenderDevice::Get().AllocateTransientShaderStorageBuffer(
                    GeometrySources.size() * sizeof(RHIRasterGeometryData));
            if (!GeometryDataBuffer)
                return std::unexpected(GeometryDataBuffer.error().Append(
                    "Raster geometry table transient buffer allocation failed"));
            if (auto R = Parameters.SetTransientShaderStorageBuffer("g_rasterDraw.geometries", *GeometryDataBuffer); !R)
                return std::unexpected(R.error().Append("Raster geometry table binding failed"));

            std::vector<RasterIndirectCommand> IndirectCommands;
            IndirectCommands.reserve(Instances.size());
            for (Uint32 InstanceIndex = 0; InstanceIndex < Instances.size(); ++InstanceIndex)
                IndirectCommands.emplace_back(RasterIndirectCommand{
                    .VertexCount   = GeometrySources[InstanceIndex].IndexCount,
                    .InstanceCount = 1,
                    .FirstVertex   = 0,
                    .FirstInstance = InstanceIndex,
                });
            GeometryPass.WriteRasterGeometryData(*GeometryDataBuffer, std::move(GeometrySources));
            auto IndirectBuffer = RHIRenderDevice::Get().AllocateTransientShaderStorageBuffer(
                std::as_bytes(std::span{IndirectCommands}).size_bytes());
            if (!IndirectBuffer)
                return std::unexpected(IndirectBuffer.error().Append(
                    "Raster indirect command transient buffer allocation failed"));
            if (auto R = GeometryPass.WriteTransientShaderStorageBuffer(
                    *IndirectBuffer, std::as_bytes(std::span{IndirectCommands})); !R)
                return std::unexpected(R.error().Append("Raster indirect command buffer write failed"));

            const auto MaterialRecords = MaterialMgr.GetMaterialRecords();
            const auto MaterialDataBytes = std::as_bytes(MaterialRecords);
            auto       MaterialDataBuffer =
                RHIRenderDevice::Get().AllocateTransientShaderStorageBuffer(MaterialDataBytes.size_bytes());
            if (!MaterialDataBuffer)
                return std::unexpected(MaterialDataBuffer.error().Append(
                    "Raster geometry material-data transient buffer allocation failed"));
            if (auto R = GeometryPass.WriteTransientShaderStorageBuffer(*MaterialDataBuffer, MaterialDataBytes); !R)
                return std::unexpected(R.error().Append("Raster geometry material-data transient buffer write failed"));
            if (auto R = Parameters.SetTransientShaderStorageBuffer("g_rasterDraw.materials", *MaterialDataBuffer); !R)
                return std::unexpected(R.error().Append("Raster geometry material-data storage buffer binding failed"));

            GeometryPass.BindShaderParameters(
                PipelineRef,
                Parameters,
                RHIShaderParameterResources{
                    .Samplers = {SamplerLinearRef, SamplerAnisoRef},
                });
            for (Uint32 InstanceIndex = 0; InstanceIndex < IndirectCommands.size(); ++InstanceIndex) {
                const RasterInstanceIndex PushData{.Value = InstanceIndex};
                GeometryPass.PushConstants(PipelineRef, 0, &PushData, sizeof(PushData));
                GeometryPass.DrawIndirect(
                    PipelineRef, *IndirectBuffer, InstanceIndex * sizeof(RasterIndirectCommand), 1);
            }
        }

        auto DeferredLightBuffer = RHIRenderDevice::Get().AllocateTransientShaderStorageBuffer(LightBytes.size_bytes());
        if (!DeferredLightBuffer)
            return std::unexpected(
                DeferredLightBuffer.error().Append("Deferred lighting light buffer allocation failed"));
        auto DeferredFrameBuffer =
            RHIRenderDevice::Get().AllocateTransientConstantBuffer(sizeof(DeferredFrameConstants));
        if (!DeferredFrameBuffer)
            return std::unexpected(
                DeferredFrameBuffer.error().Append("Deferred lighting frame buffer allocation failed"));
        auto DeferredViewBuffer = RHIRenderDevice::Get().AllocateTransientConstantBuffer(sizeof(DeferredViewConstants));
        if (!DeferredViewBuffer)
            return std::unexpected(
                DeferredViewBuffer.error().Append("Deferred lighting view buffer allocation failed"));
        const DeferredFrameConstants DeferredFrame{
            .ExposureEV100 = View.ExposureEV100,
            .LightCount    = static_cast<Uint32>(Scene.Lights.size()),
        };
        const DeferredViewConstants DeferredView{
            .InverseViewProjection = hlslpp::inverse(View.ViewProjection),
            .CameraPosition        = hlslpp::interop::float4{hlslpp::float4{
                View.CameraPosition.x, View.CameraPosition.y, View.CameraPosition.z, 1.0f}},
            .ViewportSize          = hlslpp::interop::float2{hlslpp::float2{static_cast<float>(AlbedoRTRef->GetWidth()),
                                                                            static_cast<float>(AlbedoRTRef->GetHeight())}},
        };
        const auto DeferredMaterialRecords = MaterialMgr.GetMaterialRecords();
        const auto DeferredMaterialDataBytes = std::as_bytes(DeferredMaterialRecords);
        auto       DeferredMaterialBuffer =
            RHIRenderDevice::Get().AllocateTransientShaderStorageBuffer(DeferredMaterialDataBytes.size_bytes());
        if (!DeferredMaterialBuffer)
            return std::unexpected(
                DeferredMaterialBuffer.error().Append("Deferred lighting material buffer allocation failed"));

        RHIPass LightingPass{
            .Desc =
                RHIRenderingDesc{
                    .ColorAttachments =
                        {
                            {.TextureRef = SceneColorRTRef,
                             .ClearValue = {.R = 0.025f, .G = 0.035f, .B = 0.055f, .A = 1.0f}},
                        },
                },
        };
        if (auto R = LightingPass.WriteTransientShaderStorageBuffer(*DeferredLightBuffer, LightBytes); !R)
            return std::unexpected(R.error().Append("Deferred lighting light buffer write failed"));
        if (auto R = LightingPass.WriteTransientShaderStorageBuffer(*DeferredMaterialBuffer, DeferredMaterialDataBytes);
            !R)
            return std::unexpected(R.error().Append("Deferred lighting material buffer write failed"));
        if (auto R = LightingPass.WriteTransientConstantBuffer(*DeferredFrameBuffer,
                                                               std::as_bytes(std::span{&DeferredFrame, 1}));
            !R)
            return std::unexpected(R.error().Append("Deferred lighting frame buffer write failed"));
        if (auto R = LightingPass.WriteTransientConstantBuffer(*DeferredViewBuffer,
                                                               std::as_bytes(std::span{&DeferredView, 1}));
            !R)
            return std::unexpected(R.error().Append("Deferred lighting view buffer write failed"));
        auto DeferredParameters = RHIShaderParameters::Create(*DeferredPipelineRef);
        if (auto R = DeferredParameters.SetSampledRenderTarget("g_gbuffer.albedo", AlbedoRTRef); !R)
            return std::unexpected(R.error().Append("Deferred lighting albedo binding failed"));
        if (auto R = DeferredParameters.SetSampledRenderTarget("g_gbuffer.normal", NormalRTRef); !R)
            return std::unexpected(R.error().Append("Deferred lighting normal binding failed"));
        if (auto R = DeferredParameters.SetSampledRenderTarget("g_gbuffer.materialId", MaterialIdRTRef); !R)
            return std::unexpected(R.error().Append("Deferred lighting material ID binding failed"));
        if (auto R = DeferredParameters.SetSampledRenderTarget("g_gbuffer.entityId", EntityIdRTRef); !R)
            return std::unexpected(R.error().Append("Deferred lighting entity ID binding failed"));
        if (auto R = DeferredParameters.SetSampledRenderTarget("g_gbuffer.depth", DepthRTRef); !R)
            return std::unexpected(R.error().Append("Deferred lighting depth binding failed"));
        if (auto R = DeferredParameters.SetResourceArray("g_textures.uTextures", Textures); !R)
            return std::unexpected(R.error().Append("Deferred lighting texture parameter binding failed"));
        if (auto R = DeferredParameters.SetSampler("g_samplers.uSamplerLinear", SamplerLinearRef); !R)
            return std::unexpected(R.error().Append("Deferred lighting linear sampler binding failed"));
        if (auto R = DeferredParameters.SetSampler("g_samplers.uSamplerAniso", SamplerAnisoRef); !R)
            return std::unexpected(R.error().Append("Deferred lighting anisotropic sampler binding failed"));
        if (auto R = DeferredParameters.SetTransientConstantBuffer("g_deferred.frame", *DeferredFrameBuffer); !R)
            return std::unexpected(R.error().Append("Deferred lighting frame binding failed"));
        if (auto R = DeferredParameters.SetTransientConstantBuffer("g_deferred.view", *DeferredViewBuffer); !R)
            return std::unexpected(R.error().Append("Deferred lighting view binding failed"));
        if (auto R =
                DeferredParameters.SetTransientShaderStorageBuffer("g_deferred.materials", *DeferredMaterialBuffer);
            !R)
            return std::unexpected(R.error().Append("Deferred lighting material buffer binding failed"));
        if (auto R = DeferredParameters.SetTransientShaderStorageBuffer("g_deferred.lights", *DeferredLightBuffer); !R)
            return std::unexpected(R.error().Append("Deferred lighting lights binding failed"));
        LightingPass.SetFullViewport();
        LightingPass.SetFullScissorRect();
        LightingPass.SetGraphicsPipeline(m_DeferredPipeline);
        LightingPass.BindShaderParameters(
            m_DeferredPipeline,
            std::move(DeferredParameters),
            RHIShaderParameterResources{
                .RenderTargets = {AlbedoRTRef, NormalRTRef, MaterialIdRTRef, EntityIdRTRef, DepthRTRef},
            });
        LightingPass.Draw(m_DeferredPipeline);

        CmdList.Scopes.push_back(std::move(GeometryPass));
        CmdList.Scopes.push_back(std::move(LightingPass));
        if (Scene.SelectedPixel) {
            auto EditorSelectionPass =
                BuildEditorSelectionPass(View, *Scene.SelectedPixel, EditorSelectionPipelineRef);
            if (!EditorSelectionPass)
                return std::unexpected(EditorSelectionPass.error().Append(
                    "Editor selection post-process pass construction failed"));
            CmdList.Scopes.push_back(std::move(*EditorSelectionPass));
        }
        return {};
    }

    [[nodiscard]] auto GetOrRequestMesh(StringView Asset) -> ResourceRef<ResourceMesh>& {
        for (auto& Entry : m_MeshCache) {
            if (Entry.Asset == Asset)
                return Entry.Mesh;
        }

        auto& Entry = m_MeshCache.emplace_back(RasterMeshCacheEntry{
            .Asset = String(Asset),
            .Mesh  = ResourceManager::Get().RequestMeshRef(Asset),
        });
        return Entry.Mesh;
    }

    [[nodiscard]] auto BuildDrawInstances(const SceneSnapshot& Scene) -> std::vector<InstanceData> {
        std::vector<InstanceData> DrawInstances = {};
        auto&                     Resources     = ResourceManager::Get();
        auto&                     MaterialMgr   = MaterialManager::Get();

        for (const auto& Renderable : Scene.Meshes) {
            if (Renderable.MeshAsset.empty())
                continue;

            const auto* Mesh = Resources.TryGetReady(GetOrRequestMesh(Renderable.MeshAsset));
            if (!Mesh)
                continue;

            // Resolve material ID: prefer scene material, fallback to submesh material, then default
            Uint32 MaterialID = Renderable.MaterialId.empty() ? 0 : MaterialMgr.FindMaterialId(Renderable.MaterialId);

            for (const auto& Group : Mesh->GetMeshGroups()) {
                for (const auto& SubMesh : Group.SubMeshes) {
                    auto PositionVB  = SubMesh.PositionVB;
                    auto NormalVB    = SubMesh.NormalVB;
                    auto TangentVB   = SubMesh.TangentVB;
                    auto UVVB        = SubMesh.UVVB;
                    auto IndexBuffer = SubMesh.IB;
                    if (!PositionVB || !NormalVB || !TangentVB || !IndexBuffer)
                        continue;
                    if (!UVVB)
                        UVVB = PositionVB;

                    // If scene material not found, try submesh material
                    Uint32 SubMeshMaterialID = MaterialID;
                    if (SubMeshMaterialID == 0 && SubMesh.MaterialId != 0) {
                        SubMeshMaterialID = SubMesh.MaterialId;
                    }

                    DrawInstances.emplace_back(InstanceData{
                        .EntityId       = Renderable.EntityId,
                        .PositionVB     = std::move(PositionVB),
                        .NormalVB       = std::move(NormalVB),
                        .TangentVB      = std::move(TangentVB),
                        .UVVB           = std::move(UVVB),
                        .IndexBuffer    = std::move(IndexBuffer),
                        .MaterialID     = SubMeshMaterialID,
                        .HasUV0         = SubMesh.HasUV0,
                        .HasTangents    = SubMesh.HasTangents,
                        .WorldTransform = Renderable.WorldTransform,
                        .BoundingSphere = BuildWorldBoundingSphere(SubMesh.Positions, Renderable.WorldTransform),
                    });
                }
            }
        }

        return DrawInstances;
    }

    [[nodiscard]] static auto BuildFrameConstants(Float32 Time, Float32 ExposureEV100, Uint32 LightCount)
        -> RasterFrameConstants {
        return RasterFrameConstants{.Time = Time, .ExposureEV100 = ExposureEV100, .LightCount = LightCount};
    }

    [[nodiscard]] static auto BuildLightData(std::span<const LightSnapshot> Lights) -> std::vector<LightGpuData> {
        std::vector<LightGpuData> Result = {};
        Result.reserve(std::max<std::size_t>(Lights.size(), 1));
        for (const auto& Light : Lights) {
            Result.emplace_back(LightGpuData{
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
    [[nodiscard]] static auto BuildWorldBoundingSphere(const std::vector<hlslpp::interop::float3>& Positions,
                                                       const hlslpp::float4x4&                     WorldTransform)
        -> hlslpp::interop::float4 {
        if (Positions.empty())
            return hlslpp::interop::float4{hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f}};

        hlslpp::float3 Min = hlslpp::float3{Positions.front().x, Positions.front().y, Positions.front().z};
        hlslpp::float3 Max = Min;
        for (const auto& Position : Positions) {
            const float PositionX = Position.x;
            const float PositionY = Position.y;
            const float PositionZ = Position.z;
            Min.x                 = std::min(static_cast<float>(Min.x), PositionX);
            Min.y                 = std::min(static_cast<float>(Min.y), PositionY);
            Min.z                 = std::min(static_cast<float>(Min.z), PositionZ);
            Max.x                 = std::max(static_cast<float>(Max.x), PositionX);
            Max.y                 = std::max(static_cast<float>(Max.y), PositionY);
            Max.z                 = std::max(static_cast<float>(Max.z), PositionZ);
        }

        const auto LocalCenter   = (Min + Max) * 0.5f;
        float      RadiusSquared = 0.0f;
        for (const auto& Position : Positions) {
            const auto Offset = hlslpp::float3{Position.x, Position.y, Position.z} - LocalCenter;
            RadiusSquared     = std::max(RadiusSquared, static_cast<float>(hlslpp::dot(Offset, Offset)));
        }

        const auto WorldCenter =
            hlslpp::mul(hlslpp::float4{LocalCenter.x, LocalCenter.y, LocalCenter.z, 1.0f}, WorldTransform);
        float LinearTransformSquared = 0.0f;
        for (Uint32 Row = 0; Row < 3; ++Row) {
            LinearTransformSquared += WorldTransform[Row].x * WorldTransform[Row].x;
            LinearTransformSquared += WorldTransform[Row].y * WorldTransform[Row].y;
            LinearTransformSquared += WorldTransform[Row].z * WorldTransform[Row].z;
        }

        return hlslpp::interop::float4{hlslpp::float4{
            WorldCenter.x, WorldCenter.y, WorldCenter.z, std::sqrt(RadiusSquared * LinearTransformSquared)}};
    }

    [[nodiscard]] static auto BuildInstanceData(const InstanceData& Instance,
                                                   Uint32                 MaterialIndex,
                                                   Uint32                 GeometryID) -> InstanceGpuData {
        return InstanceGpuData{
            .WorldTransform = Instance.WorldTransform,
            .BoundingSphere = Instance.BoundingSphere,
            .MaterialIndex  = MaterialIndex,
            .EntityId       = Instance.EntityId,
            .GeometryID     = GeometryID,
        };
    }

    [[nodiscard]] static auto BuildViewConstants(const RenderViewSnapshot& View) -> RasterViewConstants {
        return RasterViewConstants{
            .ViewProjection = View.ViewProjection,
            .CameraPosition = hlslpp::interop::float4{hlslpp::float4{
                View.CameraPosition.x, View.CameraPosition.y, View.CameraPosition.z, 1.0f}},
        };
    }

    auto GetViewParameters(const RenderViewSnapshot& View, const RHIGraphicsPipeline& Pipeline)
        -> RHIShaderParameters& {
        auto* ViewRenderTargetPtr = View.Targets.GBuffer.AlbedoRT.operator->();
        for (auto& State : m_ViewParameters) {
            if (State.ViewRenderTargetPtr != ViewRenderTargetPtr)
                continue;
            if (State.Parameters.GetLayoutId() != Pipeline.GetShaderParameterLayout().GetId())
                State.Parameters = RHIShaderParameters::Create(Pipeline);
            return State.Parameters;
        }

        auto& State = m_ViewParameters.emplace_back(RasterViewParameterState{
            .ViewRenderTargetPtr = ViewRenderTargetPtr,
            .Parameters          = RHIShaderParameters::Create(Pipeline),
        });
        return State.Parameters;
    }

    RHIRef<RHIGraphicsPipeline>           m_Pipeline                = nullptr;
    RHIRef<RHIGraphicsPipeline>           m_EditorSelectionPipeline = nullptr;
    RHIRef<RHIGraphicsPipeline>           m_DeferredPipeline         = nullptr;
    RHIRef<RHISampler>                    m_SamplerLinear     = nullptr;
    RHIRef<RHISampler>                    m_SamplerAniso      = nullptr;
    std::vector<RasterMeshCacheEntry>     m_MeshCache         = {};
    std::vector<RasterViewParameterState> m_ViewParameters    = {};
};

RendererFactory::AutoRegistrar<RasterRenderer> RegRasterRenderer{"Raster"};

} // namespace SoulEngine