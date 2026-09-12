module;

// needed for offsetof
#include <cstddef>
#include <entt/entt.hpp>
#include <hlsl++.h>

export module Renderer:RayTracingRenderer;

import Core;
import EditorTypes;
import Material;
import RHI;
import RenderGraph;
import Scene;

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

/// @brief RenderGraph-driven hardware path-tracing renderer.
///
/// Per frame: the GeometryUploader lazily uploads geometry; every BLAS it
/// requests joins the frame's pending-build AS work alongside a freshly
/// created hollow TLAS (no culling — secondary rays need off-screen geometry),
/// which the device builds ahead of every pass; PathTracingPass traces into
/// a transient output target and PresentBlitPass copies the result into
/// SceneColorRT carrying the frame Present.
class RayTracingRenderer final : public IRenderer {
  public:
    RayTracingRenderer() = default;
    ~RayTracingRenderer() override {
        OnDetach();
    }

    [[nodiscard]] auto OnAttach() -> std::expected<void, ErrorMessage> override {
        auto& Registry = PipelineRegistry::Get();
        Registry.Register<PathTracingPass>();
        Registry.Register<PresentBlitPass>();

        auto SamplerLinear = RHIRenderDevice::Get().CreateSampler("Renderer/RayTracing/Sampler/Linear",
                                                                  {.Profile = RHISamplerProfile::LinearRepeat});
        if (!SamplerLinear)
            return std::unexpected(SamplerLinear.error().Append("Ray-tracing linear sampler creation failed"));
        m_SamplerLinear = std::move(*SamplerLinear);
        return {};
    }

    auto OnDetach() -> void override {
        m_SamplerLinear = {};
    }

    [[nodiscard]] auto Render(const GameSnapshot& Scene, const EditorSnapshot& Editor)
        -> std::expected<RenderResult, ErrorMessage> override {
        RenderResult Result = {};
        std::vector<CameraViewRecord> Views = Scene.Views;
        Views.reserve(Scene.Views.size() + Editor.Views.size());
        for (const auto& EditorView : Editor.Views)
            Views.emplace_back(EditorView.ToCameraViewRecord());
        if (Views.empty())
            return Result;
        const auto& View = Views.front();

        auto* ViewNormal   = View.Targets.GBuffer.NormalRT.TryGet();
        auto* ViewEntityId = View.Targets.GBuffer.EntityIdRT.TryGet();
        auto* SceneColor   = View.Targets.SceneColorRT.TryGet();
        if (!ViewNormal || !ViewEntityId || !SceneColor)
            return Result;

        // ── Filter RT-ready instances, then build the shared scene tables ──
        const auto Instances = FilterInstances(Scene.Instances);
        if (Instances.empty())
            return Result;
        const auto DrawData = SceneGpuData::Create(Instances, Scene.Lights, Scene.Textures);

        // ── Per-frame hollow TLAS: the desc slot order is the canonical    ──
        // ── instance order (InstanceIndex() mapping); the device allocates ──
        // ── storage and builds at frame start, ahead of every pass         ──
        std::vector<RHITopLevelAccelerationStructureInstance> TlasInstances = {};
        TlasInstances.reserve(DrawData.InstanceRecords.size());
        for (const auto& Instance : DrawData.InstanceRecords)
            TlasInstances.push_back(RHITopLevelAccelerationStructureInstance{
                .Blas      = DrawData.GeometryBlas[Instance.GeometryID],
                .Transform = Instance.WorldTransform,
            });
        auto Tlas = RHIRenderDevice::Get().CreateTopLevelAccelerationStructure(
            "Renderer/RayTracing/Tlas",
            RHITopLevelAccelerationStructureDesc{.Instances = std::move(TlasInstances)});
        if (!Tlas)
            return std::unexpected(Tlas.error().Append("Ray-tracing TLAS creation failed"));

        // ── Frame constants ──
        const auto FrameData = RayTracingFrameConstants{
            .Common = RendererFrameConstants{
                .Time          = Scene.Time,
                .ExposureEV100 = View.ExposureEV100,
                .LightCount    = static_cast<Uint32>(Scene.Lights.size()),
            },
            .PathSettings = hlslpp::interop::float4{hlslpp::float4{
                0.0f, static_cast<Float32>(kPathTracingMaxBounces), 0.0f, 0.0f}},
        };
        const auto ViewData = RendererViewConstants{View};

        // ── Build the frame graph ──
        RenderGraph Graph;

        const auto InstancesHandle = Graph.CreateShaderStorageBuffer(
            "RT/Instances",
            RGShaderStorageBufferDesc{.SizeBytes   = std::as_bytes(std::span{DrawData.InstanceRecords}).size(),
                                      .InitialData = std::as_bytes(std::span{DrawData.InstanceRecords})});
        const auto GeometriesHandle = Graph.CreateShaderStorageBuffer(
            "RT/Geometries",
            RGShaderStorageBufferDesc{.SizeBytes   = std::as_bytes(std::span{DrawData.GeometryRecords}).size(),
                                      .InitialData = std::as_bytes(std::span{DrawData.GeometryRecords})});
        const auto MaterialsHandle = Graph.CreateShaderStorageBuffer(
            "RT/Materials",
            RGShaderStorageBufferDesc{.SizeBytes   = std::as_bytes(std::span{DrawData.MaterialRecords}).size(),
                                      .InitialData = std::as_bytes(std::span{DrawData.MaterialRecords})});
        const auto LightsHandle = Graph.CreateShaderStorageBuffer(
            "RT/Lights",
            RGShaderStorageBufferDesc{.SizeBytes   = std::as_bytes(std::span{DrawData.LightRecords}).size(),
                                      .InitialData = std::as_bytes(std::span{DrawData.LightRecords})});
        const auto FrameHandle = Graph.CreateConstantBuffer(
            "RT/Frame",
            RGConstantBufferDesc{.SizeBytes   = std::as_bytes(std::span{&FrameData, 1}).size(),
                                 .InitialData = std::as_bytes(std::span{&FrameData, 1})});
        const auto ViewHandle = Graph.CreateConstantBuffer(
            "RT/View",
            RGConstantBufferDesc{.SizeBytes   = std::as_bytes(std::span{&ViewData, 1}).size(),
                                 .InitialData = std::as_bytes(std::span{&ViewData, 1})});

        // Create the output target once: PathTracing (UAV write) and
        // PresentBlit (SRV read) must reference the same graph resource so
        // the graph sees the dependency edge between them.
        const auto OutputHandle = Graph.CreateTexture(
            "RT/Output",
            RGTextureDesc{.Width  = ViewNormal->GetWidth(),
                          .Height = ViewNormal->GetHeight(),
                          .Format = RHIFormat::B8G8R8A8_UNORM,
                          .AllowShaderStorage = true});
        PathTracingPass::Parameter TraceParameter = {};
        TraceParameter.TlasRef          = *Tlas;
        TraceParameter.Output         = RGStorageTextureUAV{.Texture = OutputHandle};
        TraceParameter.PrimaryNormal  = RGStorageTextureUAV{.Texture = Graph.Import(View.Targets.GBuffer.NormalRT)};
        TraceParameter.PrimaryEntityId =
            RGStorageTextureUAV{.Texture = Graph.Import(View.Targets.GBuffer.EntityIdRT)};
        TraceParameter.Instances    = RGStorageBufferSRV{.Buffer = InstancesHandle};
        TraceParameter.Geometries   = RGStorageBufferSRV{.Buffer = GeometriesHandle};
        TraceParameter.Materials    = RGStorageBufferSRV{.Buffer = MaterialsHandle};
        TraceParameter.Lights       = RGStorageBufferSRV{.Buffer = LightsHandle};
        TraceParameter.Frame        = RGConstantBufferSRV{.Buffer = FrameHandle};
        TraceParameter.View         = RGConstantBufferSRV{.Buffer = ViewHandle};
        TraceParameter.LinearSampler = m_SamplerLinear;
        TraceParameter.Textures      = Scene.Textures;
        TraceParameter.Extent =
            PathTracingExtent{.Width = ViewNormal->GetWidth(), .Height = ViewNormal->GetHeight()};
        Graph.AddPass<PathTracingPass>(std::move(TraceParameter));

        Graph.AddPass<PresentBlitPass>(PresentBlitPass::Parameter{
            .Source     = RGTextureSRV{.Texture = OutputHandle},
            .SceneColor = RGColorRT{.Texture = Graph.Import(View.Targets.SceneColorRT), .Present = true},
        });

        auto PassList = Graph.Compile();
        if (!PassList)
            return std::unexpected(PassList.error().Append("RayTracingRenderer frame graph compile failed"));
        Result.CmdList = std::move(*PassList);

        // Frame-start AS work: the device executes the pending BLAS batch,
        // then this frame's TLAS build, ahead of every pass.
        Result.PendingBlasBuilds = m_Uploader.DrainPendingBlasBuilds();
        Result.PendingTlasBuilds = {std::move(*Tlas)};
        return Result;
    }

  private:
    /// @brief Upload-gate the frame's instances: queue lazy buffer and BLAS
    /// uploads on first encounter, then keep only GPU-ready instances for
    /// this frame's tables.
    [[nodiscard]] auto FilterInstances(std::span<const InstanceRecord> SourceInstances)
        -> std::vector<InstanceRecord> {
        std::vector<InstanceRecord> Result = {};
        Result.reserve(SourceInstances.size());
        for (const auto& Renderable : SourceInstances) {
            if (!Renderable.Geometry)
                continue;
            auto& Record = *Renderable.Geometry;

            // we try to request to build blas here
            if (!Record.IsUploadRequested(true)) {
                if (auto R = m_Uploader.Upload(Record, true); !R) {
                    LogWarning("RayTracingRenderer: geometry upload failed: {}", R.error().ToString());
                    continue;
                }
            }
            if (!Record.IsReadyOnGpu(true))
                continue;

            Result.emplace_back(Renderable);
        }
        return Result;
    }

    GeometryUploader                          m_Uploader              = {};
    RHIRef<RHISampler>                        m_SamplerLinear = nullptr;
};

RendererFactory::AutoRegistrar<RayTracingRenderer> RegRayTracingRenderer{"RayTracing"};

} // namespace SoulEngine
