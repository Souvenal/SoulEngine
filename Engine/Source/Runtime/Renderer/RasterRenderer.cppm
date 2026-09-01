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
import :Common;
import :PostProcess.EditorPostProcess;
import :Raster;

export import std;

export namespace SoulEngine {

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
struct alignas(16) RasterIndirectCommand {
    Uint32 VertexCount   = 0;
    Uint32 InstanceCount = 1;
    Uint32 FirstVertex   = 0;
    Uint32 FirstInstance = 0;
};
static_assert(sizeof(RasterIndirectCommand) == sizeof(Uint32) * 4);

/// @brief Current-frame data shared by every camera geometry pass.
struct RasterFrameDrawData {
    std::vector<InstanceRecord::GpuData>  Instances           = {};
    std::vector<GeometryRecord::GpuData> GeometryRecords     = {};
    std::vector<MaterialRecord::GpuData>  MaterialRecords     = {};
    std::vector<RasterIndirectCommand>   IndirectCommands    = {};

    RHIRef<RHITransientShaderStorageBuffer> InstanceBuffer = nullptr;
    RHIRef<RHITransientShaderStorageBuffer> GeometryBuffer = nullptr;
    RHIRef<RHITransientShaderStorageBuffer> IndirectBuffer = nullptr;
    RHIRef<RHITransientShaderStorageBuffer> MaterialBuffer = nullptr;

    [[nodiscard]] static auto Create(std::span<const InstanceRecord> SourceInstances,
                                     const RHIRefArray<RHISampledTexture>& Textures)
        -> std::expected<RasterFrameDrawData, ErrorMessage> {
        RasterFrameDrawData Result = {};
        std::unordered_map<const GeometryRecord*, Uint32> GeometryIDs = {};
        std::unordered_map<const MaterialRecord*, Uint32> MaterialIDs = {};
        std::vector<Uint32>                               IndexCounts = {};
        GeometryIDs.reserve(SourceInstances.size());
        MaterialIDs.reserve(SourceInstances.size());
        IndexCounts.reserve(SourceInstances.size());
        Result.Instances.reserve(SourceInstances.size());
        Result.MaterialRecords.emplace_back(MaterialRecord::GpuData{});

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
            Result.Instances.emplace_back(SourceInstance.BuildGpuData(GeometryID, MaterialID));
        }

        // The deferred pass samples material index 0 even for an empty scene,
        // so the material table buffer always exists.
        auto MaterialBuffer =
            RHIRenderDevice::Get().CreateTransientShaderStorageBuffer(RHITransientShaderStorageBufferDesc{
                .Data = std::as_bytes(std::span{Result.MaterialRecords}),
            });
        if (!MaterialBuffer)
            return std::unexpected(
                MaterialBuffer.error().Append("Raster frame material-data transient allocation failed"));
        Result.MaterialBuffer = *MaterialBuffer;


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
    }

    [[nodiscard]] auto Render(const SceneSnapshot& Scene)
        -> std::expected<RenderResult, ErrorMessage> override {
        RenderResult Result = {};
        if (Scene.Views.empty())
            return Result;

        auto DrawData = RasterFrameDrawData::Create(Scene.Instances, Scene.Textures);
        if (!DrawData)
            return std::unexpected(DrawData.error().Append("Raster frame draw-data construction failed"));

        for (const auto& View : Scene.Views) {
            if (auto R = RenderView(Result.CmdList, *DrawData, View, Scene); !R)
                return std::unexpected(R.error().Append("Raster geometry view rendering failed"));
        }

        return Result;
    }

  private:
    [[nodiscard]] auto RenderView(RenderPassList&             CmdList,
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
            !SamplerLinearRef || !SamplerAnisoRef)
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

        const auto ViewData = RendererViewConstants{View};
        auto ViewBuffer = RHIRenderDevice::Get().CreateTransientConstantBuffer(RHITransientConstantBufferDesc{
            .Data = std::as_bytes(std::span{&ViewData, 1}),
        });
        if (!ViewBuffer)
            return std::unexpected(
                ViewBuffer.error().Append("Raster geometry view transient constant allocation failed"));


        if (!DrawData.Instances.empty()) {
            auto Pass = std::make_unique<GeometryPass>(PipelineRef);
            Pass->SetInput(GeometryPassInput{
                .Albedo = AlbedoRTRef,
                .Normal = NormalRTRef,
                .MaterialId = MaterialIdRTRef,
                .EntityId = EntityIdRTRef,
                .Depth = DepthRTRef,
                .LinearSampler = SamplerLinearRef,
                .AnisotropicSampler = SamplerAnisoRef,
                .Textures = Scene.Textures,
                .LightBuffer = *LightBuffer,
                .FrameBuffer = *FrameBuffer,
                .ViewBuffer = *ViewBuffer,
                .InstanceBuffer = DrawData.InstanceBuffer,
                .GeometryBuffer = DrawData.GeometryBuffer,
                .MaterialBuffer = DrawData.MaterialBuffer,
                .IndirectBuffer = DrawData.IndirectBuffer,
                .DrawCount = static_cast<Uint32>(DrawData.IndirectCommands.size()),
            });
            CmdList.Passes.push_back(std::move(Pass));
        }

        auto DeferredFrameBuffer =
            RHIRenderDevice::Get().CreateTransientConstantBuffer(RHITransientConstantBufferDesc{
                .Data = std::as_bytes(std::span{&FrameData, 1}),
            });
        if (!DeferredFrameBuffer)
            return std::unexpected(
                DeferredFrameBuffer.error().Append("Deferred lighting frame buffer allocation failed"));
        auto DeferredViewBuffer =
            RHIRenderDevice::Get().CreateTransientConstantBuffer(RHITransientConstantBufferDesc{
                .Data = std::as_bytes(std::span{&ViewData, 1}),
            });
        if (!DeferredViewBuffer)
            return std::unexpected(
                DeferredViewBuffer.error().Append("Deferred lighting view buffer allocation failed"));
        auto LightingPass = std::make_unique<DeferredLightingPass>(m_DeferredPipeline);
        LightingPass->SetInput(DeferredLightingPassInput{
                .SceneColor = SceneColorRTRef,
                .Albedo = AlbedoRTRef,
                .Normal = NormalRTRef,
                .MaterialId = MaterialIdRTRef,
                .EntityId = EntityIdRTRef,
                .Depth = DepthRTRef,
                .LinearSampler = SamplerLinearRef,
                .AnisotropicSampler = SamplerAnisoRef,
                .Textures = Scene.Textures,
                .FrameBuffer = *DeferredFrameBuffer,
                .ViewBuffer = *DeferredViewBuffer,
                .MaterialBuffer = DrawData.MaterialBuffer,
                .LightBuffer = *LightBuffer,
            });
        CmdList.Passes.push_back(std::move(LightingPass));
        if (Scene.SelectedPixel) {
            auto EditorSelectionPass =
                BuildEditorSelectionPass(View, *Scene.SelectedPixel, EditorSelectionPipelineRef);
            if (!EditorSelectionPass)
                return std::unexpected(
                    EditorSelectionPass.error().Append("Editor selection post-process pass construction failed"));
            auto& PresentPass = static_cast<IRHIGraphicsPass&>(**EditorSelectionPass);
            PresentPass.SetPresentOutput();
            CmdList.Passes.push_back(std::move(*EditorSelectionPass));
        } else {
            static_cast<IRHIGraphicsPass&>(*CmdList.Passes.back()).SetPresentOutput();
        }
        return {};
    }

    [[nodiscard]] static auto BuildFrameConstants(Float32 Time, Float32 ExposureEV100, Uint32 LightCount)
        -> RendererFrameConstants {
        return RendererFrameConstants{.Time = Time, .ExposureEV100 = ExposureEV100, .LightCount = LightCount};
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
    RHIRef<RHIGraphicsPipeline>           m_Pipeline                = nullptr;
    RHIRef<RHIGraphicsPipeline>           m_EditorSelectionPipeline = nullptr;
    RHIRef<RHIGraphicsPipeline>           m_DeferredPipeline        = nullptr;
    RHIRef<RHISampler>                    m_SamplerLinear           = nullptr;
    RHIRef<RHISampler>                    m_SamplerAniso            = nullptr;
};

RendererFactory::AutoRegistrar<RasterRenderer> RegRasterRenderer{"Raster"};

} // namespace SoulEngine
