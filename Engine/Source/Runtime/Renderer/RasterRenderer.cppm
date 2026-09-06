module;

// needed for offsetof
#include <cstddef>
#include <entt/entity/entity.hpp>
#include <hlsl++.h>

export module Renderer:RasterRenderer;

import Core;
import EditorTypes;
import Material;
import Resource;
import RHI;
import Scene;
import TaskGraph;

import :IRenderer;
import :Common;
import :EditorPasses;
import :RasterPasses;

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
        const auto ShaderPath = ConfigManager::Get().EngineShadersDirPath() / "Raster" / "RasterGeometry.slang";

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

        const auto DeferredShaderPath = ConfigManager::Get().EngineShadersDirPath() / "Raster" / "DeferredLighting.slang";
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

        const auto SelectionMaskShaderPath = ConfigManager::Get().EngineShadersDirPath() / "Editor" / "SelectionMask.slang";
        auto       SelectionMaskPipelineRequest = RequestGraphicsPipeline(
            "SelectionMaskPass",
            GraphicsPipelineRequest{
                .VertEntry = {.SourcePath = SelectionMaskShaderPath, .EntryPoint = "vertMain"},
                .FragEntry = {.SourcePath = SelectionMaskShaderPath, .EntryPoint = "fragMain"},
                .ColorFormats = {RHIFormat::R8_UNORM},
            });
        if (!SelectionMaskPipelineRequest)
            return std::unexpected(
                SelectionMaskPipelineRequest.error().Append("Selection mask pipeline request failed"));
        m_SelectionMaskPipeline = std::move(*SelectionMaskPipelineRequest);

        const auto SelectionOutlineShaderPath = ConfigManager::Get().EngineShadersDirPath() / "Editor" / "SelectionOutline.slang";
        auto       SelectionOutlinePipelineRequest = RequestGraphicsPipeline(
            "SelectionOutlinePass",
            GraphicsPipelineRequest{
                .VertEntry    = {.SourcePath = SelectionOutlineShaderPath, .EntryPoint = "vertMain"},
                .FragEntry    = {.SourcePath = SelectionOutlineShaderPath, .EntryPoint = "fragMain"},
                .Blend        =
                    RHIBlendState{.Attachments = {RHIBlendAttachment{
                        .BlendEnable         = true,
                        .SrcColorBlendFactor = RHIBlendFactor::SrcAlpha,
                        .DstColorBlendFactor = RHIBlendFactor::OneMinusSrcAlpha,
                        .ColorBlendOp        = RHIBlendOp::Add,
                        .SrcAlphaBlendFactor = RHIBlendFactor::One,
                        .DstAlphaBlendFactor = RHIBlendFactor::Zero,
                        .AlphaBlendOp        = RHIBlendOp::Add,
                    }}},
                .ColorFormats = {RHIFormat::B8G8R8A8_UNORM},
            });
        if (!SelectionOutlinePipelineRequest)
            return std::unexpected(
                SelectionOutlinePipelineRequest.error().Append("Selection outline pipeline request failed"));
        m_SelectionOutlinePipeline = std::move(*SelectionOutlinePipelineRequest);

        const auto CullingShaderPath = ConfigManager::Get().EngineShadersDirPath() / "Raster" / "CullingCompute.slang";
        auto       CullingPipelineRequest = RequestComputePipeline(
            "CullingPass",
            ComputePipelineRequest{
                .ComputeEntry = {.SourcePath = CullingShaderPath, .EntryPoint = "cullMain"},
            });
        if (!CullingPipelineRequest)
            return std::unexpected(
                CullingPipelineRequest.error().Append("Culling compute pipeline request failed"));
        m_CullingPipeline = std::move(*CullingPipelineRequest);

        const auto SelectionFilterShaderPath =
            ConfigManager::Get().EngineShadersDirPath() / "Editor" / "SelectionFilter.slang";
        auto SelectionFilterPipelineRequest = RequestComputePipeline(
            "SelectionFilterPass",
            ComputePipelineRequest{
                .ComputeEntry = {.SourcePath = SelectionFilterShaderPath, .EntryPoint = "filterMain"},
            });
        if (!SelectionFilterPipelineRequest)
            return std::unexpected(
                SelectionFilterPipelineRequest.error().Append("Selection filter compute pipeline request failed"));
        m_SelectionFilterPipeline = std::move(*SelectionFilterPipelineRequest);

        return {};
    }

    auto OnDetach() -> void override {
        m_Pipeline                 = {};
        m_DeferredPipeline         = {};
        m_SelectionMaskPipeline    = {};
        m_SelectionOutlinePipeline = {};
        m_SelectionFilterPipeline  = {};
        m_CullingPipeline          = {};
        m_SamplerLinear            = {};
        m_SamplerAniso             = {};
    }

    [[nodiscard]] auto Render(const GameSnapshot& Scene, const EditorSnapshot& Editor)
        -> std::expected<RenderResult, ErrorMessage> override {
        RenderResult Result = {};
        if (Scene.Views.empty() && Editor.Views.empty())
            return Result;

        const auto CheckPipeline = []<typename T>(const RHIRef<T>& Pipeline, StringView Name)
            -> std::expected<bool, ErrorMessage> {
            if (Pipeline.GetState() == RHIRefState::Ready)
                return true;
            if (Pipeline.GetState() == RHIRefState::Failed) {
                if (const auto Error = Pipeline.GetError())
                    return std::unexpected(Error->Append(Format("Raster pipeline '{}' failed", Name)));
                return std::unexpected(ErrorMessage(Format("Raster pipeline '{}' failed without an error", Name)));
            }
            return false;
        };
        const auto RequireReady = [&CheckPipeline](const auto& Pipeline, StringView Name)
            -> std::expected<bool, ErrorMessage> {
            return CheckPipeline(Pipeline, Name);
        };
        for (const auto& [Pipeline, Name] : std::array{
                 std::pair{std::cref(m_Pipeline), StringView{"GeometryPass"}},
                 std::pair{std::cref(m_DeferredPipeline), StringView{"DeferredLightingPass"}},
             }) {
            const auto Ready = RequireReady(Pipeline.get(), Name);
            if (!Ready)
                return std::unexpected(Ready.error());
            if (!*Ready)
                return Result;
        }
        if (!Scene.Instances.empty()) {
            const auto CullingReady = RequireReady(m_CullingPipeline, "CullingPass");
            if (!CullingReady)
                return std::unexpected(CullingReady.error());
            if (!*CullingReady)
                return Result;
        }
        const bool UsesEditorSelection =
            !Scene.Instances.empty() && Editor.SelectedEntity &&
            std::ranges::any_of(Editor.Views, [](const EditorViewRecord& View) { return static_cast<bool>(View.SelectionMask); });
        if (UsesEditorSelection) {
            const auto SelectionFilterReady = RequireReady(m_SelectionFilterPipeline, "SelectionFilterPass");
            if (!SelectionFilterReady)
                return std::unexpected(SelectionFilterReady.error());
            if (!*SelectionFilterReady)
                return Result;
            const auto SelectionMaskReady = RequireReady(m_SelectionMaskPipeline, "SelectionMaskPass");
            if (!SelectionMaskReady)
                return std::unexpected(SelectionMaskReady.error());
            if (!*SelectionMaskReady)
                return Result;
            const auto SelectionOutlineReady = RequireReady(m_SelectionOutlinePipeline, "SelectionOutlinePass");
            if (!SelectionOutlineReady)
                return std::unexpected(SelectionOutlineReady.error());
            if (!*SelectionOutlineReady)
                return Result;
        }

        auto DrawData = RasterFrameDrawData::Create(Scene.Instances, Scene.Textures);
        if (!DrawData)
            return std::unexpected(DrawData.error().Append("Raster frame draw-data construction failed"));

        for (const auto& View : Scene.Views) {
            if (auto R = RenderView(Result.CmdList, *DrawData, View, Scene, Editor, nullptr); !R)
                return std::unexpected(R.error().Append("Raster geometry view rendering failed"));
        }
        for (const auto& EditorView : Editor.Views) {
            const auto View = EditorView.ToCameraViewRecord();
            if (auto R = RenderView(Result.CmdList, *DrawData, View, Scene, Editor, &EditorView); !R)
                return std::unexpected(R.error().Append("Raster editor-camera view rendering failed"));
        }

        return Result;
    }

  private:
    [[nodiscard]] auto RenderView(RenderPassList&             CmdList,
                                  const RasterFrameDrawData& DrawData,
                                  const CameraViewRecord&          View,
                                  const GameSnapshot&               Scene,
                                  const EditorSnapshot&             Editor,
                                  const EditorViewRecord*     EditorView) -> std::expected<void, ErrorMessage> {
        using magic_enum::bitwise_operators::operator|;

        const auto& AlbedoRTRef                = View.Targets.GBuffer.AlbedoRT;
        const auto& NormalRTRef                = View.Targets.GBuffer.NormalRT;
        const auto& EntityIdRTRef              = View.Targets.GBuffer.EntityIdRT;
        const auto& MaterialIdRTRef            = View.Targets.GBuffer.MaterialIdRT;
        const auto& DepthRTRef                 = View.Targets.GBuffer.DepthRT;
        const auto& SceneColorRTRef            = View.Targets.SceneColorRT;
        const auto& PipelineRef                = m_Pipeline;
        const auto& SamplerLinearRef           = m_SamplerLinear;
        const auto& SamplerAnisoRef            = m_SamplerAniso;
        const auto& DeferredPipelineRef        = m_DeferredPipeline;
        if (!AlbedoRTRef || !NormalRTRef || !EntityIdRTRef || !MaterialIdRTRef || !DepthRTRef || !SceneColorRTRef ||
             !PipelineRef || !DeferredPipelineRef || !SamplerLinearRef || !SamplerAnisoRef ||
             (!DrawData.Instances.empty() && !m_CullingPipeline) ||
             (EditorView && Editor.SelectedEntity && EditorView->SelectionMask && !DrawData.Instances.empty() &&
              (!m_SelectionFilterPipeline || !m_SelectionMaskPipeline || !m_SelectionOutlinePipeline)))
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


        RHIRef<RHITransientShaderStorageBuffer> SelectionInstanceBuffer = nullptr;
        RHIRef<RHITransientShaderStorageBuffer> SelectionIndirectBuffer = nullptr;
        RHIRef<RHITransientShaderStorageBuffer> SelectionCounterBuffer = nullptr;

        if (!DrawData.Instances.empty()) {
            // ── Per-view culling (placeholder: copies scene data) ──────
            const auto InstanceDataSize = DrawData.Instances.size() * sizeof(InstanceRecord::GpuData);
            const auto IndirectDataSize = DrawData.IndirectCommands.size() * sizeof(RasterIndirectCommand);
            const auto ZeroedInstances = std::vector<std::byte>(InstanceDataSize, std::byte{0});
            const auto ZeroedIndirect  = std::vector<std::byte>(IndirectDataSize, std::byte{0});
            const auto ZeroedCounter   = std::vector<std::byte>(sizeof(Uint32), std::byte{0});

            auto ViewInstanceBuffer =
                RHIRenderDevice::Get().CreateTransientShaderStorageBuffer(RHITransientShaderStorageBufferDesc{
                    .Data = ZeroedInstances,
                });
            if (!ViewInstanceBuffer)
                return std::unexpected(
                    ViewInstanceBuffer.error().Append("View instance buffer transient allocation failed"));

            auto ViewIndirectBuffer =
                RHIRenderDevice::Get().CreateTransientShaderStorageBuffer(RHITransientShaderStorageBufferDesc{
                    .Data = ZeroedIndirect,
                    .Usage = RHITransientBufferUsage::ShaderRead | RHITransientBufferUsage::IndirectCommandRead,
                });
            if (!ViewIndirectBuffer)
                return std::unexpected(
                    ViewIndirectBuffer.error().Append("View indirect buffer transient allocation failed"));

            auto ViewCounterBuffer =
                RHIRenderDevice::Get().CreateTransientShaderStorageBuffer(RHITransientShaderStorageBufferDesc{
                    .Data = ZeroedCounter,
                });
            if (!ViewCounterBuffer)
                return std::unexpected(
                    ViewCounterBuffer.error().Append("View counter buffer transient allocation failed"));

            auto CullPass = std::make_unique<CullingPass>(m_CullingPipeline);
            CullPass->SetInput(CullingPassInput{
                .SceneInstanceBuffer  = DrawData.InstanceBuffer,
                .SceneIndirectBuffer  = DrawData.IndirectBuffer,
                .GeometryBuffer       = DrawData.GeometryBuffer,
                .ViewInstanceBuffer   = *ViewInstanceBuffer,
                .ViewIndirectBuffer   = *ViewIndirectBuffer,
                .ViewCounterBuffer    = *ViewCounterBuffer,
                .InstanceCount        = static_cast<Uint32>(DrawData.Instances.size()),
                .CommandCount         = static_cast<Uint32>(DrawData.IndirectCommands.size()),
            });
            CmdList.Passes.push_back(std::move(CullPass));

            if (EditorView && Editor.SelectedEntity) {
                const auto SelectionInstanceBytes = std::vector<std::byte>(InstanceDataSize, std::byte{0});
                const auto SelectionIndirectBytes = std::vector<std::byte>(IndirectDataSize, std::byte{0});
                const auto SelectionCounterBytes =
                    std::vector<std::byte>(DrawData.IndirectCommands.size() * sizeof(Uint32), std::byte{0});

                auto FilterInstances =
                    RHIRenderDevice::Get().CreateTransientShaderStorageBuffer(RHITransientShaderStorageBufferDesc{
                        .Data = SelectionInstanceBytes,
                    });
                if (!FilterInstances)
                    return std::unexpected(
                        FilterInstances.error().Append("Selection filter instance buffer allocation failed"));
                SelectionInstanceBuffer = *FilterInstances;

                auto FilterIndirect =
                    RHIRenderDevice::Get().CreateTransientShaderStorageBuffer(RHITransientShaderStorageBufferDesc{
                        .Data = SelectionIndirectBytes,
                        .Usage = RHITransientBufferUsage::ShaderRead | RHITransientBufferUsage::IndirectCommandRead,
                    });
                if (!FilterIndirect)
                    return std::unexpected(
                        FilterIndirect.error().Append("Selection filter indirect buffer allocation failed"));
                SelectionIndirectBuffer = *FilterIndirect;

                auto FilterCounters =
                    RHIRenderDevice::Get().CreateTransientShaderStorageBuffer(RHITransientShaderStorageBufferDesc{
                        .Data = SelectionCounterBytes,
                    });
                if (!FilterCounters)
                    return std::unexpected(
                        FilterCounters.error().Append("Selection filter counter buffer allocation failed"));
                SelectionCounterBuffer = *FilterCounters;

                auto FilterPass = std::make_unique<SelectionFilterPass>(m_SelectionFilterPipeline);
                FilterPass->SetInput(SelectionFilterPassInput{
                    .SceneInstanceBuffer = *ViewInstanceBuffer,
                    .SceneIndirectBuffer = *ViewIndirectBuffer,
                    .SelectedInstanceBuffer = SelectionInstanceBuffer,
                    .SelectedIndirectBuffer = SelectionIndirectBuffer,
                    .CommandCounterBuffer = SelectionCounterBuffer,
                    .CommandCount = static_cast<Uint32>(DrawData.IndirectCommands.size()),
                    .SelectedEntityId = static_cast<Uint32>(entt::to_integral(*Editor.SelectedEntity)),
                });
                CmdList.Passes.push_back(std::move(FilterPass));
            }

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
                .InstanceBuffer = *ViewInstanceBuffer,
                .GeometryBuffer = DrawData.GeometryBuffer,
                .MaterialBuffer = DrawData.MaterialBuffer,
                .IndirectBuffer = *ViewIndirectBuffer,
                .DrawCount = static_cast<Uint32>(DrawData.IndirectCommands.size()),
            });
            CmdList.Passes.push_back(std::move(Pass));
        }

        // Read back the EntityId texel under the cursor after the geometry
        // pass has written the GBuffer.
        if (Editor.IsHovering && Editor.ReadbackTarget) {
            auto PickingPass = BuildEntityPickingPass(EntityIdRTRef, Editor.HoverPixel, Editor.ReadbackTarget);
            if (!PickingPass)
                return std::unexpected(PickingPass.error().Append("Entity picking pass construction failed"));
            CmdList.Passes.push_back(std::move(*PickingPass));
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
                .Textures = Scene.Textures,
                .FrameBuffer = *DeferredFrameBuffer,
                .ViewBuffer = *DeferredViewBuffer,
                .MaterialBuffer = DrawData.MaterialBuffer,
                .LightBuffer = *LightBuffer,
            });
        CmdList.Passes.push_back(std::move(LightingPass));

        // Selection outline: render selected entity mask (no depth test) then detect edges.
        if (EditorView && Editor.SelectedEntity && !DrawData.Instances.empty() &&
            EditorView->SelectionMask) {
            const auto SelectedEntityId = static_cast<Uint32>(entt::to_integral(*Editor.SelectedEntity));
            const auto& MaskRT = EditorView->SelectionMask;

            auto MaskPass = std::make_unique<SelectionMaskPass>(m_SelectionMaskPipeline);
            MaskPass->SetInput(SelectionMaskPassInput{
                .Mask = MaskRT,
                .FrameBuffer = *FrameBuffer,
                .ViewBuffer = *ViewBuffer,
                .InstanceBuffer = SelectionInstanceBuffer,
                .GeometryBuffer = DrawData.GeometryBuffer,
                .IndirectBuffer = SelectionIndirectBuffer,
                .DrawCount = static_cast<Uint32>(DrawData.IndirectCommands.size()),
            });
            CmdList.Passes.push_back(std::move(MaskPass));

            auto OutlinePass = std::make_unique<SelectionOutlinePass>(m_SelectionOutlinePipeline);
            OutlinePass->SetInput(SelectionOutlinePassInput{
                .SceneColor = SceneColorRTRef,
                .Mask = MaskRT,
                .LinearSampler = SamplerLinearRef,
                .ViewportWidth = static_cast<float>(View.GetWidth()),
                .ViewportHeight = static_cast<float>(View.GetHeight()),
            });
            CmdList.Passes.push_back(std::move(OutlinePass));
        }

        static_cast<IRHIGraphicsPass&>(*CmdList.Passes.back()).SetPresentOutput();
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
    RHIRef<RHIGraphicsPipeline>           m_Pipeline                 = nullptr;
    RHIRef<RHIGraphicsPipeline>           m_DeferredPipeline         = nullptr;
    RHIRef<RHIGraphicsPipeline>           m_SelectionMaskPipeline    = nullptr;
    RHIRef<RHIGraphicsPipeline>           m_SelectionOutlinePipeline = nullptr;
    RHIRef<RHIComputePipeline>            m_SelectionFilterPipeline  = nullptr;
    RHIRef<RHIComputePipeline>            m_CullingPipeline          = nullptr;
    RHIRef<RHISampler>                    m_SamplerLinear            = nullptr;
    RHIRef<RHISampler>                    m_SamplerAniso             = nullptr;
};

RendererFactory::AutoRegistrar<RasterRenderer> RegRasterRenderer{"Raster"};

} // namespace SoulEngine
