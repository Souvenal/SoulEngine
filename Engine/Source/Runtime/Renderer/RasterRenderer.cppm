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

/// @brief Current-frame data shared by every camera geometry pass.
struct RasterFrameDrawData {
    std::vector<InstanceRecord::GpuData>  Instances           = {};
    std::vector<GeometryRecord::GpuData> GeometryRecords     = {};
    std::vector<RasterIndirectCommand>   IndirectCommands    = {};

    RHIRef<RHITransientShaderStorageBuffer> InstanceBuffer = nullptr;
    RHIRef<RHITransientShaderStorageBuffer> GeometryBuffer = nullptr;
    RHIRef<RHITransientShaderStorageBuffer> IndirectBuffer = nullptr;

    [[nodiscard]] static auto Create(std::span<const InstanceRecord> SourceInstances)
        -> std::expected<RasterFrameDrawData, ErrorMessage> {
        RasterFrameDrawData Result = {};
        std::unordered_map<const GeometryRecord*, Uint32> GeometryIDs = {};
        std::vector<Uint32>                               IndexCounts = {};
        GeometryIDs.reserve(SourceInstances.size());
        IndexCounts.reserve(SourceInstances.size());
        Result.Instances.reserve(SourceInstances.size());

        for (const auto& SourceInstance : SourceInstances) {
            if (!SourceInstance.Geometry)
                continue;

            const auto& Geometry = *SourceInstance.Geometry;
            if (!Geometry.PositionBuffer || !Geometry.NormalBuffer || !Geometry.IndexBuffer ||
                Geometry.IndexCount == 0)
                continue;

            const auto* GeometryPtr = std::addressof(Geometry);
            const auto  [It, Inserted] =
                GeometryIDs.emplace(GeometryPtr, static_cast<Uint32>(Result.GeometryRecords.size()));
            const auto GeometryID = It->second;
            if (Inserted) {
                Result.GeometryRecords.emplace_back(Geometry.BuildGpuData());
                IndexCounts.emplace_back(Geometry.IndexCount);
            }
            Result.Instances.emplace_back(SourceInstance.BuildGpuData(GeometryID));
        }

        std::ranges::sort(Result.Instances, [](const auto& Lhs, const auto& Rhs) {
            return Lhs.GeometryID < Rhs.GeometryID;
        });

        for (Uint32 InstanceIndex = 0; InstanceIndex < Result.Instances.size();) {
            const auto GeometryID = Result.Instances[InstanceIndex].GeometryID;
            const auto GroupBegin = InstanceIndex;
            while (InstanceIndex < Result.Instances.size() &&
                   Result.Instances[InstanceIndex].GeometryID == GeometryID)
                ++InstanceIndex;

            // VertexCount is the number of vertex-shader invocations. The
            // vertex shader uses SV_VertexID to index this Geometry's index
            // buffer, so the draw count is the Geometry IndexCount.
            Result.IndirectCommands.emplace_back(RasterIndirectCommand{
                .VertexCount   = IndexCounts[GeometryID],
                .InstanceCount = InstanceIndex - GroupBegin,
                .FirstVertex   = 0,
                .FirstInstance = GroupBegin,
            });
        }

        if (Result.Instances.empty())
            return Result;

        using magic_enum::bitwise_operators::operator|;

        const auto InstanceBytes = std::as_bytes(std::span{Result.Instances});
        auto InstanceBuffer =
            RHIRenderDevice::Get().CreateTransientShaderStorageBuffer(RHITransientShaderStorageBufferDesc{
                .Data = InstanceBytes,
            });
        if (!InstanceBuffer)
            return std::unexpected(
                InstanceBuffer.error().Append("Raster geometry instance-data transient allocation failed"));
        Result.InstanceBuffer = *InstanceBuffer;

        auto GeometryBuffer =
            RHIRenderDevice::Get().CreateTransientShaderStorageBuffer(RHITransientShaderStorageBufferDesc{
                .Data = std::as_bytes(std::span{Result.GeometryRecords}),
            });
        if (!GeometryBuffer)
            return std::unexpected(
                GeometryBuffer.error().Append("Raster geometry table transient allocation failed"));
        Result.GeometryBuffer = *GeometryBuffer;

        const auto IndirectBytes = std::as_bytes(std::span{Result.IndirectCommands});
        auto IndirectBuffer =
            RHIRenderDevice::Get().CreateTransientShaderStorageBuffer(RHITransientShaderStorageBufferDesc{
                .Data = IndirectBytes,
                .Usage = RHITransientBufferUsage::ShaderRead | RHITransientBufferUsage::IndirectCommandRead,
            });
        if (!IndirectBuffer)
            return std::unexpected(
                IndirectBuffer.error().Append("Raster indirect command transient allocation failed"));
        Result.IndirectBuffer = *IndirectBuffer;

        return Result;
    }
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

        auto PipelineRequest = RequestGraphicsPipeline(
            "GeometryPass",
            GraphicsPipelineRequest{
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
                .ColorFormats = std::vector<RHIFormat>(GBuffer::ColorFormats.begin(), GBuffer::ColorFormats.end()),
                .DepthFormat  = GBuffer::DepthFormat,
            });
        if (!PipelineRequest)
            return std::unexpected(PipelineRequest.error().Append("Raster geometry graphics pipeline request failed"));
        m_Pipeline = std::move(*PipelineRequest);

        const auto DeferredShaderPath = ConfigManager::Get().EngineShadersDirPath() / "DeferredLighting.slang";
        auto       DeferredPipelineRequest =
            RequestGraphicsPipeline("DeferredLightingPass",
                                    GraphicsPipelineRequest{
                                        .VertEntry    = {.SourcePath = DeferredShaderPath, .EntryPoint = "vertMain"},
                                        .FragEntry    = {.SourcePath = DeferredShaderPath, .EntryPoint = "fragMain"},
                                        .ColorFormats = {RHIFormat::B8G8R8A8_UNORM},
                                    });
        if (!DeferredPipelineRequest)
            return std::unexpected(DeferredPipelineRequest.error().Append("Deferred lighting pipeline request failed"));
        m_DeferredPipeline = std::move(*DeferredPipelineRequest);

        auto EditorSelectionPipelineRequest = RequestEditorSelectionPipeline();
        if (!EditorSelectionPipelineRequest)
            return std::unexpected(
                EditorSelectionPipelineRequest.error().Append("Editor selection post-process pipeline request failed"));
        m_EditorSelectionPipeline = std::move(*EditorSelectionPipelineRequest);

        auto SamplerLinear = RHIRenderDevice::Get().CreateSampler("Renderer/Raster/Sampler/Linear",
                                                                  {.Profile = RHISamplerProfile::LinearRepeat});
        if (!SamplerLinear)
            return std::unexpected(SamplerLinear.error().Append("Raster linear sampler creation failed"));
        m_SamplerLinear = std::move(*SamplerLinear);

        auto SamplerAniso = RHIRenderDevice::Get().CreateSampler("Renderer/Raster/Sampler/Anisotropic",
                                                                 {.Profile = RHISamplerProfile::AnisotropicRepeat});
        if (!SamplerAniso)
            return std::unexpected(SamplerAniso.error().Append("Raster anisotropic sampler creation failed"));
        m_SamplerAniso = std::move(*SamplerAniso);

        return {};
    }

    auto OnDetach() -> void override {
        m_Pipeline                = {};
        m_EditorSelectionPipeline = {};
        m_DeferredPipeline        = {};
        m_SamplerLinear           = {};
        m_SamplerAniso            = {};
        m_ViewParameters.clear();
    }

    [[nodiscard]] auto Render(const SceneSnapshot& Scene)
        -> std::expected<RenderResult, ErrorMessage> override {
        RenderResult Result = {};
        if (Scene.Views.empty())
            return Result;

        auto DrawData = RasterFrameDrawData::Create(Scene.Instances);
        if (!DrawData)
            return std::unexpected(DrawData.error().Append("Raster frame draw-data construction failed"));

        for (const auto& View : Scene.Views) {
            if (auto R = RenderView(Result.CmdList, *DrawData, View, Scene); !R)
                return std::unexpected(R.error().Append("Raster geometry view rendering failed"));
        }

        return Result;
    }

  private:
    [[nodiscard]] auto RenderView(RHICommandList&            CmdList,
                                  const RasterFrameDrawData& DrawData,
                                  const CameraViewRecord&    View,
                                  const SceneSnapshot&       Scene) -> std::expected<void, ErrorMessage> {
        const auto& AlbedoRTRef                = View.Targets.GBuffer.AlbedoRT;
        const auto& NormalRTRef                = View.Targets.GBuffer.NormalRT;
        const auto& EntityIdRTRef              = View.Targets.GBuffer.EntityIdRT;
        const auto& MaterialIdRTRef            = View.Targets.GBuffer.MaterialIdRT;
        const auto& DepthRTRef                 = View.Targets.GBuffer.DepthRT;
        const auto& SceneColorRTRef            = View.Targets.SceneColorRT;
        const auto& PipelineRef                = m_Pipeline;
        const auto& EditorSelectionPipelineRef = m_EditorSelectionPipeline;
        const auto& SamplerLinearRef           = m_SamplerLinear;
        const auto& SamplerAnisoRef            = m_SamplerAniso;
        const auto& DeferredPipelineRef        = m_DeferredPipeline;
        if (!AlbedoRTRef || !NormalRTRef || !EntityIdRTRef || !MaterialIdRTRef || !DepthRTRef || !SceneColorRTRef ||
            !PipelineRef || !EditorSelectionPipelineRef || !SamplerLinearRef || !SamplerAnisoRef ||
            !DeferredPipelineRef)
            return {};

        const auto FrameData =
            BuildFrameConstants(Scene.Time, View.ExposureEV100, static_cast<Uint32>(Scene.Lights.size()));
        const auto Lights      = BuildLightData(Scene.Lights);
        const auto LightBytes  = std::as_bytes(std::span{Lights});
        auto LightBuffer =
            RHIRenderDevice::Get().CreateTransientShaderStorageBuffer(RHITransientShaderStorageBufferDesc{
                .Data = LightBytes,
            });
        if (!LightBuffer)
            return std::unexpected(
                LightBuffer.error().Append("Raster geometry light transient storage allocation failed"));
        auto FrameBuffer = RHIRenderDevice::Get().CreateTransientConstantBuffer(RHITransientConstantBufferDesc{
            .Data = std::as_bytes(std::span{&FrameData, 1}),
        });
        if (!FrameBuffer)
            return std::unexpected(
                FrameBuffer.error().Append("Raster geometry frame transient constant allocation failed"));

        const auto ViewData   = BuildViewConstants(View);
        auto ViewBuffer = RHIRenderDevice::Get().CreateTransientConstantBuffer(RHITransientConstantBufferDesc{
            .Data = std::as_bytes(std::span{&ViewData, 1}),
        });
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
                             .ClearValue = std::array<Float32, 4>{0.025f, 0.035f, 0.055f, 1.0f}},
                            {.TextureRef = NormalRTRef, .ClearValue = std::array<Float32, 4>{0.0f, 0.0f, 1.0f, 1.0f}},
                            {.TextureRef = MaterialIdRTRef,
                             .ClearValue = std::array<Float32, 4>{0.0f, 0.0f, 0.0f, 1.0f}},
                            {.TextureRef = EntityIdRTRef,
                             .ClearValue = std::array<Uint32, 4>{GBuffer::BackgroundEntityId, 0, 0, 0}},
                        },
                    .DepthAttachment =
                        RHIDepthAttachmentDesc{
                            .TextureRef = DepthRTRef,
                            .ClearValue = {.Depth = 1.0f, .Stencil = 0},
                        },
                },
        };
        if (CmdList.PresentSourceRef.GetState() == RHIRefState::Unknown)
            CmdList.PresentSourceRef = SceneColorRTRef;

        auto&       MaterialMgr = MaterialManager::Get();
        const auto& Textures    = MaterialMgr.GetMaterialTextures();
        if (auto R = Parameters.SetResourceArray("g_textures.uTextures", Textures); !R)
            return std::unexpected(R.error().Append("Raster geometry texture parameter binding failed"));
        const auto MaterialRecords = MaterialMgr.GetMaterialRecords();
        auto MaterialDataBuffer =
            RHIRenderDevice::Get().CreateTransientShaderStorageBuffer(RHITransientShaderStorageBufferDesc{
                .Data = std::as_bytes(MaterialRecords),
            });
        if (!MaterialDataBuffer)
            return std::unexpected(
                MaterialDataBuffer.error().Append("Raster geometry material-data transient buffer allocation failed"));
        GeometryPass.SetFullViewport();
        GeometryPass.SetFullScissorRect();
        GeometryPass.SetGraphicsPipeline(PipelineRef);
        if (!DrawData.Instances.empty()) {
            if (auto R = Parameters.SetTransientShaderStorageBuffer("g_rasterDraw.instances", DrawData.InstanceBuffer);
                !R)
                return std::unexpected(R.error().Append("Raster geometry instance-data storage buffer binding failed"));
            if (auto R = Parameters.SetTransientShaderStorageBuffer("g_rasterDraw.geometries", DrawData.GeometryBuffer);
                !R)
                return std::unexpected(R.error().Append("Raster geometry table binding failed"));

            if (auto R = Parameters.SetTransientShaderStorageBuffer("g_rasterDraw.materials", *MaterialDataBuffer); !R)
                return std::unexpected(R.error().Append("Raster geometry material-data storage buffer binding failed"));

            GeometryPass.BindShaderParameters(PipelineRef,
                                              Parameters,
                                              RHIShaderParameterResources{
                                                  .Samplers = {SamplerLinearRef, SamplerAnisoRef},
                                              });
            GeometryPass.DrawIndirect(
                PipelineRef, DrawData.IndirectBuffer, 0, static_cast<Uint32>(DrawData.IndirectCommands.size()));
        }

        const DeferredFrameConstants DeferredFrame{
            .ExposureEV100 = View.ExposureEV100,
            .LightCount    = static_cast<Uint32>(Scene.Lights.size()),
        };
        const DeferredViewConstants DeferredView{
            .InverseViewProjection = hlslpp::inverse(View.ViewProjection),
            .CameraPosition        = hlslpp::interop::float4{hlslpp::float4{
                View.CameraPosition.x, View.CameraPosition.y, View.CameraPosition.z, 1.0f}},
            .ViewportSize = hlslpp::interop::float2{hlslpp::float2{static_cast<float>(AlbedoRTRef->GetWidth()),
                                                                   static_cast<float>(AlbedoRTRef->GetHeight())}},
        };
        auto DeferredFrameBuffer =
            RHIRenderDevice::Get().CreateTransientConstantBuffer(RHITransientConstantBufferDesc{
                .Data = std::as_bytes(std::span{&DeferredFrame, 1}),
            });
        if (!DeferredFrameBuffer)
            return std::unexpected(
                DeferredFrameBuffer.error().Append("Deferred lighting frame buffer allocation failed"));
        auto DeferredViewBuffer =
            RHIRenderDevice::Get().CreateTransientConstantBuffer(RHITransientConstantBufferDesc{
                .Data = std::as_bytes(std::span{&DeferredView, 1}),
            });
        if (!DeferredViewBuffer)
            return std::unexpected(
                DeferredViewBuffer.error().Append("Deferred lighting view buffer allocation failed"));
        RHIPass LightingPass{
            .Desc =
                RHIRenderingDesc{
                    .ColorAttachments =
                        {
                            {.TextureRef = SceneColorRTRef,
                             .ClearValue = std::array<Float32, 4>{0.025f, 0.035f, 0.055f, 1.0f}},
                        },
                },
        };
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
                DeferredParameters.SetTransientShaderStorageBuffer("g_deferred.materials", *MaterialDataBuffer);
            !R)
            return std::unexpected(R.error().Append("Deferred lighting material buffer binding failed"));
        if (auto R = DeferredParameters.SetTransientShaderStorageBuffer("g_deferred.lights", *LightBuffer); !R)
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
            auto EditorSelectionPass = BuildEditorSelectionPass(View, *Scene.SelectedPixel, EditorSelectionPipelineRef);
            if (!EditorSelectionPass)
                return std::unexpected(
                    EditorSelectionPass.error().Append("Editor selection post-process pass construction failed"));
            CmdList.Scopes.push_back(std::move(*EditorSelectionPass));
        }
        return {};
    }

    [[nodiscard]] static auto BuildFrameConstants(Float32 Time, Float32 ExposureEV100, Uint32 LightCount)
        -> RasterFrameConstants {
        return RasterFrameConstants{.Time = Time, .ExposureEV100 = ExposureEV100, .LightCount = LightCount};
    }

    [[nodiscard]] static auto BuildLightData(std::span<const LightRecord> Lights) -> std::vector<LightGpuData> {
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
    [[nodiscard]] static auto BuildViewConstants(const CameraViewRecord& View) -> RasterViewConstants {
        return RasterViewConstants{
            .ViewProjection = View.ViewProjection,
            .CameraPosition = hlslpp::interop::float4{hlslpp::float4{
                View.CameraPosition.x, View.CameraPosition.y, View.CameraPosition.z, 1.0f}},
        };
    }

    auto GetViewParameters(const CameraViewRecord& View, const RHIGraphicsPipeline& Pipeline) -> RHIShaderParameters& {
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
    RHIRef<RHIGraphicsPipeline>           m_DeferredPipeline        = nullptr;
    RHIRef<RHISampler>                    m_SamplerLinear           = nullptr;
    RHIRef<RHISampler>                    m_SamplerAniso            = nullptr;
    std::vector<RasterViewParameterState> m_ViewParameters          = {};
};

RendererFactory::AutoRegistrar<RasterRenderer> RegRasterRenderer{"Raster"};

} // namespace SoulEngine
