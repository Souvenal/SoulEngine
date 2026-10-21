module;

// needed for offsetof
#include <cstddef>
#include <entt/entt.hpp>
#include <hlsl++.h>

module Renderer.Raster.RasterRenderer;

import Renderer.Common;
import Renderer.EditorPasses;
import Renderer.Interface;
import Renderer.Raster.CullingPass;
import Renderer.Raster.DeferredLightingPass;
import Renderer.Raster.GeometryPass;

import Core;
import EditorTypes;
import RHI;
import RenderGraph;
import Scene;

import std;

namespace SoulEngine {

[[nodiscard]] auto BuildRasterIndirectCommands(const SceneGpuData& DrawData) -> std::vector<RasterIndirectCommand> {
    std::vector<RasterIndirectCommand> Result = {};
    for (Uint32 InstanceIndex = 0; InstanceIndex < DrawData.InstanceRecords.size();) {
        const auto GeometryID = DrawData.InstanceRecords[InstanceIndex].GeometryID;
        const auto GroupBegin = InstanceIndex;
        while (InstanceIndex < DrawData.InstanceRecords.size() &&
               DrawData.InstanceRecords[InstanceIndex].GeometryID == GeometryID)
            ++InstanceIndex;

        // VertexCount is the number of vertex-shader invocations. The vertex shader uses SV_VertexID to index
        // this Geometry's index buffer, so the draw count is the Geometry IndexCount.
        Result.emplace_back(RasterIndirectCommand{
            .VertexCount   = DrawData.GeometryIndexCounts[GeometryID],
            .InstanceCount = InstanceIndex - GroupBegin,
            .FirstVertex   = 0,
            .FirstInstance = GroupBegin,
        });
    }
    return Result;
}

RasterRenderer::~RasterRenderer() {
    OnDetach();
}

[[nodiscard]] auto RasterRenderer::OnAttach() -> std::expected<void, ErrorMessage> {
    // Typed registration: descriptors live on each graph pass type via BuildPipelineRequest(); registry
    // keys are entt::type_hash<TPass>. The deferred lighting pass has two runtime forms (direct-present
    // and outline color-write); only the form present in a frame's graph is Ensured by that frame's
    // Compile().
    auto& Registry = PipelineRegistry::Get();
    Registry.Register<CullingPass>();
    Registry.Register<SelectionFilterPass>();
    Registry.Register<GeometryPass>();
    Registry.Register<DeferredLightingPass>();
    Registry.Register<SelectionMaskPass>();
    Registry.Register<SelectionOutlinePass>();

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

auto RasterRenderer::OnDetach() -> void {
    m_SamplerLinear = {};
    m_SamplerAniso  = {};
}

[[nodiscard]] auto RasterRenderer::Render(const GameSnapshot& Scene, const EditorSnapshot& Editor)
    -> std::expected<RenderResult, ErrorMessage> {
    RenderResult Result = {};
    if (Scene.Views.empty() && Editor.Views.empty())
        return Result;

    const auto Instances      = FilterInstances(Scene.Instances);
    auto       TextureIndices = ResolveBindlessTextureIndices(Scene.Textures);
    if (!TextureIndices)
        return std::unexpected(TextureIndices.error().Append("Raster bindless texture resolution failed"));
    const auto DrawData         = SceneGpuData::Create(Instances, Scene.Lights, *TextureIndices);
    const auto IndirectCommands = BuildRasterIndirectCommands(DrawData);

    Uint32 ViewIndex = 0;
    for (const auto& View : Scene.Views) {
        if (auto R = RenderView(Result.CmdList,
                                Format("View/Scene{}", ViewIndex++),
                                DrawData,
                                IndirectCommands,
                                View,
                                Scene,
                                Editor,
                                nullptr);
            !R)
            return std::unexpected(R.error().Append("Raster geometry view rendering failed"));
    }
    for (const auto& EditorView : Editor.Views) {
        const auto View = EditorView.ToCameraViewRecord();
        if (auto R = RenderView(Result.CmdList,
                                Format("View/Editor{}", ViewIndex++),
                                DrawData,
                                IndirectCommands,
                                View,
                                Scene,
                                Editor,
                                &EditorView);
            !R)
            return std::unexpected(R.error().Append("Raster editor-camera view rendering failed"));
    }

    return Result;
}

[[nodiscard]] auto RasterRenderer::FilterInstances(std::span<const InstanceRecord> SourceInstances)
    -> std::vector<InstanceRecord> {
    std::vector<InstanceRecord> Result = {};
    Result.reserve(SourceInstances.size());
    for (const auto& SourceInstance : SourceInstances) {
        if (!SourceInstance.Geometry)
            continue;

        // Lazy upload: queue buffer creation on first encounter, then gate on GPU readiness. Freshly
        // loaded geometry skips a frame or two while the RHI thread uploads it.
        auto& Geometry = *SourceInstance.Geometry;
        if (!Geometry.IsUploadRequested(false)) {
            if (auto R = m_Uploader.Upload(Geometry, false); !R) {
                LogWarning("RasterRenderer: geometry upload failed: {}", R.error().ToString());
                continue;
            }
        }
        if (!Geometry.IsReadyOnGpu(false))
            continue;

        Result.emplace_back(SourceInstance);
    }
    return Result;
}

[[nodiscard]] auto RasterRenderer::RenderView(RenderPassList&                        CmdList,
                                              StringView                             ViewName,
                                              const SceneGpuData&                    DrawData,
                                              std::span<const RasterIndirectCommand> IndirectCommands,
                                              const CameraViewRecord&                View,
                                              const GameSnapshot&                    Scene,
                                              const EditorSnapshot&                  Editor,
                                              const EditorViewRecord* EditorView) -> std::expected<void, ErrorMessage> {
    // View targets are created on the graph below; the only readiness gate left is the renderer-owned
    // samplers.
    const auto& SamplerLinearRef = m_SamplerLinear;
    const auto& SamplerAnisoRef  = m_SamplerAniso;
    if (!SamplerLinearRef || !SamplerAnisoRef)
        return {};

    // Feature gates only — pipeline readiness is Compile's job (Ensure + Pending prune + Ready ref
    // stash). Do not snapshot GetState here.
    const bool HasInstances = !DrawData.InstanceRecords.empty();
    const bool EditorSel    = EditorView && Editor.SelectedEntity && HasInstances;
    // Outline follows the feature gate (not readiness). If the outline pipeline is still Pending at
    // Compile, the whole lighting -> outline -> blit chain drops (the NeverPrune blit's last-writer
    // dependency is the outline) and the frame compiles to an empty pass list — the graph's soft-degrade
    // contract.
    const bool WantOutline  = EditorSel;
    // RenderGraph wiring

    RenderGraphBuilder ViewGraph{};

    // View targets are pooled graph transients. Creation usage is explicit in the descriptor (see
    // RasterViewTargets::Create); a pass's view declaration must stay a subset of it.
    const auto Targets = RasterViewTargets::Create(ViewGraph, ViewName, View.GetWidth(), View.GetHeight());

    const auto FrameData =
        BuildFrameConstants(Scene.Time, View.ExposureEV100, static_cast<Uint32>(Scene.Lights.size()));
    const auto ViewData = RendererViewConstants{View};

    const auto MaterialBytes = std::as_bytes(std::span{DrawData.MaterialRecords});
    const auto MaterialTable =
        ViewGraph.CreateBuffer("FrameMaterials",
                               RHIBufferDesc{.SizeBytes = static_cast<Uint64>(MaterialBytes.size()),
                                             .Usage     = RHIBufferUsage::Storage,
                                             .Lifetime  = RHILifetime::Transient},
                               MaterialBytes);

    const auto FrameCB = ViewGraph.CreateBuffer("FrameConstants",
                                                RHIBufferDesc{.SizeBytes = static_cast<Uint64>(sizeof(FrameData)),
                                                              .Usage     = RHIBufferUsage::Constant,
                                                              .Lifetime  = RHILifetime::Transient},
                                                std::as_bytes(std::span{&FrameData, 1}));
    const auto ViewCB  = ViewGraph.CreateBuffer("ViewConstants",
                                                RHIBufferDesc{.SizeBytes = static_cast<Uint64>(sizeof(ViewData)),
                                                              .Usage     = RHIBufferUsage::Constant,
                                                              .Lifetime  = RHILifetime::Transient},
                                                std::as_bytes(std::span{&ViewData, 1}));
    const auto DeferredFrameCB =
        ViewGraph.CreateBuffer("DeferredFrameConstants",
                               RHIBufferDesc{.SizeBytes = static_cast<Uint64>(sizeof(FrameData)),
                                             .Usage     = RHIBufferUsage::Constant,
                                             .Lifetime  = RHILifetime::Transient},
                               std::as_bytes(std::span{&FrameData, 1}));
    const auto DeferredViewCB = ViewGraph.CreateBuffer("DeferredViewConstants",
                                                       RHIBufferDesc{.SizeBytes = static_cast<Uint64>(sizeof(ViewData)),
                                                                     .Usage     = RHIBufferUsage::Constant,
                                                                     .Lifetime  = RHILifetime::Transient},
                                                       std::as_bytes(std::span{&ViewData, 1}));

    RGBuffer SceneInstances = {};
    RGBuffer SceneIndirect  = {};
    RGBuffer GeometryTable  = {};
    if (HasInstances) {
        const auto InstanceBytes = std::as_bytes(std::span{DrawData.InstanceRecords});
        const auto GeometryBytes = std::as_bytes(std::span{DrawData.GeometryRecords});
        const auto IndirectBytes = std::as_bytes(IndirectCommands);
        SceneInstances = ViewGraph.CreateBuffer("SceneInstances",
                                                RHIBufferDesc{.SizeBytes = static_cast<Uint64>(InstanceBytes.size()),
                                                              .Usage     = RHIBufferUsage::Storage,
                                                              .Lifetime  = RHILifetime::Transient},
                                                InstanceBytes);
        GeometryTable  = ViewGraph.CreateBuffer("SceneGeometry",
                                                RHIBufferDesc{.SizeBytes = static_cast<Uint64>(GeometryBytes.size()),
                                                              .Usage     = RHIBufferUsage::Storage,
                                                              .Lifetime  = RHILifetime::Transient},
                                                GeometryBytes);
        SceneIndirect =
            ViewGraph.CreateBuffer("SceneIndirect",
                                   RHIBufferDesc{.SizeBytes = static_cast<Uint64>(IndirectBytes.size()),
                                                 .Usage     = RHIBufferUsage::Storage | RHIBufferUsage::Indirect,
                                                 .Lifetime  = RHILifetime::Transient},
                                   IndirectBytes);
    }

    // Empty light list still needs a live SSBO binding for deferred lighting; a zeroed 4-byte InitialData
    // keeps the resource contentful.
    static constexpr std::array<std::byte, sizeof(Uint32)> ZeroLightSlot{};
    const auto                                             LightBytes = std::as_bytes(std::span{DrawData.LightRecords});
    const auto                                             LightTable = ViewGraph.CreateBuffer(
        "FrameLights",
        RHIBufferDesc{.SizeBytes = LightBytes.empty() ? sizeof(Uint32) : static_cast<Uint64>(LightBytes.size()),
                      .Usage     = RHIBufferUsage::Storage,
                      .Lifetime  = RHILifetime::Transient},
        LightBytes.empty() ? std::span<const std::byte>{ZeroLightSlot} : LightBytes);

    // The GBuffer handles are shared between Geometry (the writer, inside the instance block below) and
    // Lighting/EntityPicking/Outline (the readers, registered after it).
    const auto Albedo     = Targets.GBuffer.Albedo;
    const auto Normal     = Targets.GBuffer.Normal;
    const auto MaterialId = Targets.GBuffer.MaterialId;
    const auto EntityId   = Targets.GBuffer.EntityId;
    const auto Depth      = Targets.GBuffer.Depth;

    // Selection filter outputs live at view scope: the mask pass consumes them and is registered outside
    // the instance block.
    RGBuffer SelectedInstances = {};
    RGBuffer SelectedIndirect  = {};

    if (HasInstances) {
        const auto InstanceDataSize = DrawData.InstanceRecords.size() * sizeof(InstanceRecord::GpuData);
        const auto IndirectDataSize = IndirectCommands.size() * sizeof(RasterIndirectCommand);

        const auto ViewInstances = ViewGraph.CreateBuffer("ViewInstances",
                                                          RHIBufferDesc{.SizeBytes = InstanceDataSize,
                                                                        .Usage     = RHIBufferUsage::Storage,
                                                                        .Lifetime  = RHILifetime::Transient});
        const auto ViewIndirect =
            ViewGraph.CreateBuffer("ViewIndirect",
                                   RHIBufferDesc{.SizeBytes = IndirectDataSize,
                                                 .Usage     = RHIBufferUsage::Storage | RHIBufferUsage::Indirect,
                                                 .Lifetime  = RHILifetime::Transient});
        const auto ViewCounter = ViewGraph.CreateBuffer("ViewCounter",
                                                        RHIBufferDesc{.SizeBytes = sizeof(Uint32),
                                                                      .Usage     = RHIBufferUsage::Storage,
                                                                      .Lifetime  = RHILifetime::Transient});

        ViewGraph.AddPass<CullingPass>({
            .ViewInstances  = ViewInstances,
            .ViewIndirect   = ViewIndirect,
            .ViewCounter    = ViewCounter,
            .SceneInstances = SceneInstances,
            .SceneIndirect  = SceneIndirect,
            .GeometryTable  = GeometryTable,
            .InstanceCount  = static_cast<Uint32>(DrawData.InstanceRecords.size()),
            .CommandCount   = static_cast<Uint32>(IndirectCommands.size()),
        });

        if (EditorView && Editor.SelectedEntity) {
            SelectedInstances = ViewGraph.CreateBuffer("SelectedInstances",
                                                       RHIBufferDesc{.SizeBytes = InstanceDataSize,
                                                                     .Usage     = RHIBufferUsage::Storage,
                                                                     .Lifetime  = RHILifetime::Transient});
            SelectedIndirect =
                ViewGraph.CreateBuffer("SelectedIndirect",
                                       RHIBufferDesc{.SizeBytes = IndirectDataSize,
                                                     .Usage     = RHIBufferUsage::Storage | RHIBufferUsage::Indirect,
                                                     .Lifetime  = RHILifetime::Transient});
            // The filter shader accumulates per-command instance counts via InterlockedAdd, so the
            // counters must start at zero every frame.
            const std::vector<std::byte> ZeroCounters(IndirectCommands.size() * sizeof(Uint32), std::byte{0});
            const auto SelectedCounters = ViewGraph.CreateBuffer("SelectedCounters",
                                                                 RHIBufferDesc{.SizeBytes = ZeroCounters.size(),
                                                                               .Usage     = RHIBufferUsage::Storage,
                                                                               .Lifetime  = RHILifetime::Transient},
                                                                 ZeroCounters);

            ViewGraph.AddPass<SelectionFilterPass>({
                .ViewInstances     = ViewInstances,
                .ViewIndirect      = ViewIndirect,
                .SelectedInstances = SelectedInstances,
                .SelectedIndirect  = SelectedIndirect,
                .SelectedCounters  = SelectedCounters,
                .CommandCount      = static_cast<Uint32>(IndirectCommands.size()),
                .SelectedEntityId  = static_cast<Uint32>(entt::to_integral(*Editor.SelectedEntity)),
            });
        }

        ViewGraph.AddPass<GeometryPass>({
            .Albedo             = Albedo,
            .Normal             = Normal,
            .MaterialId         = MaterialId,
            .EntityId           = EntityId,
            .Depth              = Depth,
            .InstanceBuffer     = ViewInstances,
            .IndirectBuffer     = ViewIndirect,
            .GeometryBuffer     = GeometryTable,
            .MaterialBuffer     = MaterialTable,
            .FrameBuffer        = FrameCB,
            .ViewBuffer         = ViewCB,
            .LinearSampler      = SamplerLinearRef,
            .AnisotropicSampler = SamplerAnisoRef,
            .DrawCount          = static_cast<Uint32>(IndirectCommands.size()),
        });
    }

    if (Editor.IsHovering && Editor.ReadbackTarget) {
        ViewGraph.AddPass<EntityPickingPass>({
            .EntityId = EntityId,
            .Readback = Editor.ReadbackTarget,
            .Pixel    = Editor.HoverPixel,
        });
    }

    const auto SceneColor = Targets.SceneColor;
    // Lighting writes SceneColor; the explicit BlitToSwapchainPass below reads it (with the outline RMW
    // in between when the feature is on).
    ViewGraph.AddPass<DeferredLightingPass>({
        .SceneColor         = SceneColor,
        .Albedo             = Albedo,
        .Normal             = Normal,
        .MaterialId         = MaterialId,
        .EntityId           = EntityId,
        .Depth              = Depth,
        .MaterialBuffer     = MaterialTable,
        .FrameBuffer        = DeferredFrameCB,
        .ViewBuffer         = DeferredViewCB,
        .LightBuffer        = LightTable,
        .LinearSampler      = SamplerLinearRef,
        .AnisotropicSampler = SamplerAnisoRef,
    });

    if (WantOutline) {
        // The selection mask is a pooled transient like any other view target: the mask pass's
        // RenderTarget write and the outline pass's Sampled read derive its usage union.
        const auto Mask =
            ViewGraph.CreateTexture(Format("{}/SelectionMask", ViewName),
                                    RHITextureDesc{.Width  = View.GetWidth(),
                                                   .Height = View.GetHeight(),
                                                   .Format = RHIFormat::R8_UNORM,
                                                   .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::Sampled});
        ViewGraph.AddPass<SelectionMaskPass>({
            .Mask              = Mask,
            .SelectedInstances = SelectedInstances,
            .SelectedIndirect  = SelectedIndirect,
            .GeometryTable     = GeometryTable,
            .MaterialTable     = MaterialTable,
            .FrameBuffer       = FrameCB,
            .ViewBuffer        = ViewCB,
            .LinearSampler     = SamplerLinearRef,
            .DrawCount         = static_cast<Uint32>(IndirectCommands.size()),
        });
        ViewGraph.AddPass<SelectionOutlinePass>({
            .SceneColor     = SceneColor,
            .Mask           = Mask,
            .LinearSampler  = SamplerLinearRef,
            .ViewportWidth  = static_cast<Float32>(View.GetWidth()),
            .ViewportHeight = static_cast<Float32>(View.GetHeight()),
        });
    }

    // Present is explicit: blit the view's final SceneColor into the swapchain. Reading SceneColor orders
    // the blit after lighting (and after the outline RMW when enabled) via the last-writer chain;
    // NeverPrune keeps it alive without consumers.
    ViewGraph.AddPass<BlitToSwapchainPass>({
        .Source    = SceneColor,
        .DstX      = View.Viewport.X,
        .DstY      = View.Viewport.Y,
        .DstWidth  = View.GetWidth(),
        .DstHeight = View.GetHeight(),
    });

    // Compile/splice anchor: the per-view graph orders, prunes, realizes, and constructs the
    // feature-gated chain in one call.
    auto ViewGraphPasses = ViewGraph.Compile();
    if (!ViewGraphPasses)
        return std::unexpected(ViewGraphPasses.error().Append("Raster view render-graph compile failed"));
    CmdList.Entries.insert(CmdList.Entries.end(),
                           std::make_move_iterator(ViewGraphPasses->Entries.begin()),
                           std::make_move_iterator(ViewGraphPasses->Entries.end()));
    return {};
}

[[nodiscard]] auto RasterRenderer::BuildFrameConstants(Float32 Time, Float32 ExposureEV100, Uint32 LightCount)
    -> RendererFrameConstants {
    return RendererFrameConstants{.Time = Time, .ExposureEV100 = ExposureEV100, .LightCount = LightCount};
}

RendererFactory::AutoRegistrar<RasterRenderer> RegRasterRenderer{"Raster"};

} // namespace SoulEngine
