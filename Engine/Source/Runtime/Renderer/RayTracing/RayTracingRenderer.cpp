module;

// needed for offsetof
#include <cstddef>
#include <hlsl++.h>

module Renderer.RayTracing.RayTracingRenderer;

import Renderer.RayTracing.PathTracing;
import Renderer.Common;
import Renderer.Interface;

import Core;
import EditorTypes;
import RHI;
import RenderGraph;
import Scene;

import std;

namespace SoulEngine {

constexpr Uint32 kPathTracingMaxBounces = 6;

/// @brief Ray-tracing frame ABI extending shared frame data with path settings.
struct alignas(16) RayTracingFrameConstants {
    RendererFrameConstants Common                    = {};
    alignas(16) hlslpp::interop::float4 PathSettings = hlslpp::interop::float4{hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f}};
};
static_assert(sizeof(RayTracingFrameConstants) == 32);
static_assert(offsetof(RayTracingFrameConstants, Common) == 0);
static_assert(offsetof(RayTracingFrameConstants, PathSettings) == 16);

RayTracingRenderer::~RayTracingRenderer() {
    OnDetach();
}

[[nodiscard]] auto RayTracingRenderer::OnAttach() -> std::expected<void, ErrorMessage> {
    auto& Registry = PipelineRegistry::Get();
    Registry.Register<PathTracingPass>();

    auto SamplerLinear = RHIRenderDevice::Get().CreateSampler("Renderer/RayTracing/Sampler/Linear",
                                                              {.Profile = RHISamplerProfile::LinearRepeat});
    if (!SamplerLinear)
        return std::unexpected(SamplerLinear.error().Append("Ray-tracing linear sampler creation failed"));
    m_SamplerLinear = std::move(*SamplerLinear);
    return {};
}

auto RayTracingRenderer::OnDetach() -> void {
    m_SamplerLinear = {};
}

[[nodiscard]] auto RayTracingRenderer::Render(const GameSnapshot& Scene, const EditorSnapshot& Editor)
    -> std::expected<RenderResult, ErrorMessage> {
    RenderResult                  Result = {};
    std::vector<CameraViewRecord> Views  = Scene.Views;
    Views.reserve(Scene.Views.size() + Editor.Views.size());
    for (const auto& EditorView : Editor.Views)
        Views.emplace_back(EditorView.ToCameraViewRecord());
    if (Views.empty())
        return Result;
    const auto& View = Views.front();

    // No camera-owned targets anymore: the viewport extent is the only input the view carries.
    const Uint32 ViewWidth  = View.GetWidth();
    const Uint32 ViewHeight = View.GetHeight();
    if (ViewWidth == 0 || ViewHeight == 0)
        return Result;

    // Filter RT-ready instances, then build the shared scene tables
    const auto Instances = FilterInstances(Scene.Instances);
    if (Instances.empty())
        return Result;
    auto TextureIndices = ResolveBindlessTextureIndices(Scene.Textures);
    if (!TextureIndices)
        return std::unexpected(TextureIndices.error().Append("Ray-tracing bindless texture resolution failed"));
    const auto DrawData = SceneGpuData::Create(Instances, Scene.Lights, *TextureIndices);

    // Per-frame TLAS: the desc slot order is the canonical instance order (InstanceIndex() mapping); the
    // device builds at frame start, ahead of every pass
    std::vector<RHITopLevelAccelerationStructureInstance> TlasInstances = {};
    TlasInstances.reserve(DrawData.InstanceRecords.size());
    for (const auto& Instance : DrawData.InstanceRecords)
        TlasInstances.push_back(RHITopLevelAccelerationStructureInstance{
            .Blas      = DrawData.GeometryBlas[Instance.GeometryID],
            .Transform = Instance.WorldTransform,
        });
    auto Tlas = RHIRenderDevice::Get().CreateTopLevelAccelerationStructure(
        "Renderer/RayTracing/Tlas", RHITopLevelAccelerationStructureDesc{.Instances = std::move(TlasInstances)});
    if (!Tlas)
        return std::unexpected(Tlas.error().Append("Ray-tracing TLAS creation failed"));
    // Frame-affined build task: the TLAS builds at frame start, after the BLAS batch and ahead of every pass.
    if (auto R = RHIRenderDevice::Get().BuildTlas(*Tlas); !R)
        return std::unexpected(R.error().Append("Ray-tracing TLAS build enqueue failed"));

    // Frame constants
    const auto FrameData = RayTracingFrameConstants{
        .Common =
            RendererFrameConstants{
                .Time          = Scene.Time,
                .ExposureEV100 = View.ExposureEV100,
                .LightCount    = static_cast<Uint32>(Scene.Lights.size()),
            },
        .PathSettings =
            hlslpp::interop::float4{hlslpp::float4{0.0f, static_cast<Float32>(kPathTracingMaxBounces), 0.0f, 0.0f}},
    };
    const auto ViewData = RendererViewConstants{View};

    // Build the frame graph
    RenderGraphBuilder Graph;

    const auto InstancesHandle =
        Graph.CreateBuffer("RT/Instances",
                           RHIBufferDesc{.SizeBytes = std::as_bytes(std::span{DrawData.InstanceRecords}).size(),
                                         .Usage     = RHIBufferUsage::Storage,
                                         .Lifetime  = RHILifetime::Transient},
                           std::as_bytes(std::span{DrawData.InstanceRecords}));
    const auto GeometriesHandle =
        Graph.CreateBuffer("RT/Geometries",
                           RHIBufferDesc{.SizeBytes = std::as_bytes(std::span{DrawData.GeometryRecords}).size(),
                                         .Usage     = RHIBufferUsage::Storage,
                                         .Lifetime  = RHILifetime::Transient},
                           std::as_bytes(std::span{DrawData.GeometryRecords}));
    const auto MaterialsHandle =
        Graph.CreateBuffer("RT/Materials",
                           RHIBufferDesc{.SizeBytes = std::as_bytes(std::span{DrawData.MaterialRecords}).size(),
                                         .Usage     = RHIBufferUsage::Storage,
                                         .Lifetime  = RHILifetime::Transient},
                           std::as_bytes(std::span{DrawData.MaterialRecords}));
    const auto LightsHandle =
        Graph.CreateBuffer("RT/Lights",
                           RHIBufferDesc{.SizeBytes = std::as_bytes(std::span{DrawData.LightRecords}).size(),
                                         .Usage     = RHIBufferUsage::Storage,
                                         .Lifetime  = RHILifetime::Transient},
                           std::as_bytes(std::span{DrawData.LightRecords}));
    const auto FrameHandle =
        Graph.CreateBuffer("RT/Frame",
                           RHIBufferDesc{.SizeBytes = std::as_bytes(std::span{&FrameData, 1}).size(),
                                         .Usage     = RHIBufferUsage::Constant,
                                         .Lifetime  = RHILifetime::Transient},
                           std::as_bytes(std::span{&FrameData, 1}));
    const auto ViewHandle = Graph.CreateBuffer("RT/View",
                                               RHIBufferDesc{.SizeBytes = std::as_bytes(std::span{&ViewData, 1}).size(),
                                                             .Usage     = RHIBufferUsage::Constant,
                                                             .Lifetime  = RHILifetime::Transient},
                                               std::as_bytes(std::span{&ViewData, 1}));

    // Create the output target once: PathTracing (UAV write) and the present blit (copy source) must
    // reference the same graph resource so the graph sees the dependency edge between them; the usage
    // union covers both roles.
    const auto OutputHandle =
        Graph.CreateTexture("RT/Output",
                            RHITextureDesc{.Width  = ViewWidth,
                                           .Height = ViewHeight,
                                           .Format = RHIFormat::B8G8R8A8_UNORM,
                                           .Usage  = RHITextureUsage::Storage | RHITextureUsage::TransferSrc});
    // Primary-surface outputs are pooled graph transients too: the trace pass's UAV writes fit their
    // declared Storage usage.
    const auto PrimaryNormalHandle   = Graph.CreateTexture("RT/PrimaryNormal",
                                                           RHITextureDesc{.Width  = ViewWidth,
                                                                          .Height = ViewHeight,
                                                                          .Format = RHIFormat::R16G16B16A16_SFLOAT,
                                                                          .Usage  = RHITextureUsage::Storage});
    const auto PrimaryEntityIdHandle = Graph.CreateTexture("RT/PrimaryEntityId",
                                                           RHITextureDesc{.Width  = ViewWidth,
                                                                          .Height = ViewHeight,
                                                                          .Format = RHIFormat::R32_UINT,
                                                                          .Usage  = RHITextureUsage::Storage});
    Graph.AddPass<PathTracingPass>({
        .Output          = OutputHandle,
        .PrimaryNormal   = PrimaryNormalHandle,
        .PrimaryEntityId = PrimaryEntityIdHandle,
        .Instances       = InstancesHandle,
        .Geometries      = GeometriesHandle,
        .Materials       = MaterialsHandle,
        .Lights          = LightsHandle,
        .Frame           = FrameHandle,
        .View            = ViewHandle,
        .TlasRef         = *Tlas,
        .LinearSampler   = m_SamplerLinear,
        .Extent          = {.Width = ViewWidth, .Height = ViewHeight},
    });

    Graph.AddPass<BlitToSwapchainPass>({
        .Source    = OutputHandle,
        .DstX      = View.Viewport.X,
        .DstY      = View.Viewport.Y,
        .DstWidth  = ViewWidth,
        .DstHeight = ViewHeight,
    });

    auto PassList = Graph.Compile();
    if (!PassList)
        return std::unexpected(PassList.error().Append("RayTracingRenderer frame graph compile failed"));
    Result.CmdList = std::move(*PassList);

    return Result;
}

[[nodiscard]] auto RayTracingRenderer::FilterInstances(std::span<const InstanceRecord> SourceInstances)
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

RendererFactory::AutoRegistrar<RayTracingRenderer> RegRayTracingRenderer{"RayTracing"};

} // namespace SoulEngine
