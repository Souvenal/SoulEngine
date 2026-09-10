module;

// needed for offsetof
#include <cstddef>
#include <entt/entt.hpp>
#include <hlsl++.h>

export module Renderer:RasterRenderer;

import Core;
import EditorTypes;
import Material;
import RHI;
import RenderGraph;
import Scene;

import :IRenderer;
import :Common;
import :EditorPasses;
import :RasterPasses;

export import std;

export namespace SoulEngine {

struct alignas(16) RasterIndirectCommand {
    Uint32 VertexCount   = 0;
    Uint32 InstanceCount = 1;
    Uint32 FirstVertex   = 0;
    Uint32 FirstInstance = 0;
};
static_assert(sizeof(RasterIndirectCommand) == sizeof(Uint32) * 4);

/// @brief Current-frame CPU tables shared by every camera view.
/// GPU buffers are graph-created per ViewGraph via Create*Buffer + InitialData.
struct RasterFrameDrawData {
    std::vector<InstanceRecord::GpuData> Instances        = {};
    std::vector<GeometryRecord::GpuData> GeometryRecords  = {};
    std::vector<MaterialRecord::GpuData> MaterialRecords  = {};
    std::vector<RasterIndirectCommand>   IndirectCommands = {};

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
        // Deferred lighting samples material index 0 even for an empty scene.
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
        // Typed registration: descriptors live on each graph pass type via
        // BuildPipelineRequest(); registry keys are entt::type_hash<TPass>.
        // The deferred lighting pass has two runtime forms (direct-present
        // and outline color-write); only the form present in a frame's graph is
        // Ensured by that frame's Compile().
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

    auto OnDetach() -> void override {
        m_SamplerLinear = {};
        m_SamplerAniso  = {};
    }

    [[nodiscard]] auto Render(const GameSnapshot& Scene, const EditorSnapshot& Editor)
        -> std::expected<RenderResult, ErrorMessage> override {
        RenderResult Result = {};
        if (Scene.Views.empty() && Editor.Views.empty())
            return Result;

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

        const auto& AlbedoRTRef     = View.Targets.GBuffer.AlbedoRT;
        const auto& NormalRTRef     = View.Targets.GBuffer.NormalRT;
        const auto& EntityIdRTRef   = View.Targets.GBuffer.EntityIdRT;
        const auto& MaterialIdRTRef = View.Targets.GBuffer.MaterialIdRT;
        const auto& DepthRTRef      = View.Targets.GBuffer.DepthRT;
        const auto& SceneColorRTRef = View.Targets.SceneColorRT;
        const auto& SamplerLinearRef = m_SamplerLinear;
        const auto& SamplerAnisoRef  = m_SamplerAniso;
        if (!AlbedoRTRef || !NormalRTRef || !EntityIdRTRef || !MaterialIdRTRef || !DepthRTRef || !SceneColorRTRef ||
            !SamplerLinearRef || !SamplerAnisoRef)
            return {};

        // Feature gates only — pipeline readiness is Compile's job (Ensure +
        // Pending prune + Ready ref stash). Do not snapshot GetState here.
        const bool HasInstances = !DrawData.Instances.empty();
        const bool EditorSel    = EditorView && Editor.SelectedEntity && HasInstances &&
                               static_cast<bool>(EditorView->SelectionMask);
        // Outline Present policy follows the feature gate (not readiness). If
        // the outline pipeline is still Pending at Compile, its side-effect
        // chain cannot be built and the frame compiles to an empty pass list
        // — the graph's soft-degrade contract.
        const bool WantOutline  = EditorSel;

        // ── RenderGraph wiring ───────────────────────────────────────────
        RenderGraph ViewGraph{};

        const auto FrameData =
            BuildFrameConstants(Scene.Time, View.ExposureEV100, static_cast<Uint32>(Scene.Lights.size()));
        const auto Lights = BuildLightGpuData(Scene.Lights);
        const auto ViewData = RendererViewConstants{View};

        const auto MaterialBytes = std::as_bytes(std::span{DrawData.MaterialRecords});
        const auto MaterialTable = ViewGraph.CreateShaderStorageBuffer(
            "FrameMaterials",
            RGShaderStorageBufferDesc{
                .SizeBytes = static_cast<Uint64>(MaterialBytes.size()),
                .Stride = static_cast<Uint32>(sizeof(MaterialRecord::GpuData)),
                .Usage = RHITransientBufferUsage::ShaderRead,
                .InitialData = MaterialBytes,
            });

        const auto FrameCB = ViewGraph.CreateConstantBuffer(
            "FrameConstants",
            RGConstantBufferDesc{
                .SizeBytes = static_cast<Uint64>(sizeof(FrameData)),
                .InitialData = std::as_bytes(std::span{&FrameData, 1}),
            });
        const auto ViewCB = ViewGraph.CreateConstantBuffer(
            "ViewConstants",
            RGConstantBufferDesc{
                .SizeBytes = static_cast<Uint64>(sizeof(ViewData)),
                .InitialData = std::as_bytes(std::span{&ViewData, 1}),
            });
        const auto DeferredFrameCB = ViewGraph.CreateConstantBuffer(
            "DeferredFrameConstants",
            RGConstantBufferDesc{
                .SizeBytes = static_cast<Uint64>(sizeof(FrameData)),
                .InitialData = std::as_bytes(std::span{&FrameData, 1}),
            });
        const auto DeferredViewCB = ViewGraph.CreateConstantBuffer(
            "DeferredViewConstants",
            RGConstantBufferDesc{
                .SizeBytes = static_cast<Uint64>(sizeof(ViewData)),
                .InitialData = std::as_bytes(std::span{&ViewData, 1}),
            });

        RGStorageBufferHandle SceneInstances = {};
        RGStorageBufferHandle SceneIndirect  = {};
        RGStorageBufferHandle GeometryTable  = {};
        if (HasInstances) {
            const auto InstanceBytes = std::as_bytes(std::span{DrawData.Instances});
            const auto GeometryBytes = std::as_bytes(std::span{DrawData.GeometryRecords});
            const auto IndirectBytes = std::as_bytes(std::span{DrawData.IndirectCommands});
            SceneInstances = ViewGraph.CreateShaderStorageBuffer(
                "SceneInstances",
                RGShaderStorageBufferDesc{
                    .SizeBytes = static_cast<Uint64>(InstanceBytes.size()),
                    .Stride = static_cast<Uint32>(sizeof(InstanceRecord::GpuData)),
                    .Usage = RHITransientBufferUsage::ShaderRead,
                    .InitialData = InstanceBytes,
                });
            GeometryTable = ViewGraph.CreateShaderStorageBuffer(
                "SceneGeometry",
                RGShaderStorageBufferDesc{
                    .SizeBytes = static_cast<Uint64>(GeometryBytes.size()),
                    .Stride = static_cast<Uint32>(sizeof(GeometryRecord::GpuData)),
                    .Usage = RHITransientBufferUsage::ShaderRead,
                    .InitialData = GeometryBytes,
                });
            SceneIndirect = ViewGraph.CreateShaderStorageBuffer(
                "SceneIndirect",
                RGShaderStorageBufferDesc{
                    .SizeBytes = static_cast<Uint64>(IndirectBytes.size()),
                    .Stride = static_cast<Uint32>(sizeof(RasterIndirectCommand)),
                    .Usage = RHITransientBufferUsage::ShaderRead | RHITransientBufferUsage::IndirectCommandRead,
                    .InitialData = IndirectBytes,
                });
        }

        // Empty light list still needs a live SSBO binding for deferred lighting;
        // a zeroed 4-byte InitialData keeps the resource contentful.
        static constexpr std::array<std::byte, sizeof(Uint32)> ZeroLightSlot{};
        const auto LightBytes = std::as_bytes(std::span{Lights});
        const auto LightTable = ViewGraph.CreateShaderStorageBuffer(
            "FrameLights",
            RGShaderStorageBufferDesc{
                .SizeBytes = LightBytes.empty() ? sizeof(Uint32) : static_cast<Uint64>(LightBytes.size()),
                .Usage = RHITransientBufferUsage::ShaderRead,
                .InitialData = LightBytes.empty()
                                   ? std::optional<std::span<const std::byte>>{ZeroLightSlot}
                                   : std::optional<std::span<const std::byte>>{LightBytes},
            });

        // The GBuffer render targets are shared between Geometry (the writer,
        // inside the instance block below) and Lighting/EntityPicking/Outline
        // (the readers, registered after it), so they are imported once here.
        const auto Albedo     = ViewGraph.Import(AlbedoRTRef);
        const auto Normal     = ViewGraph.Import(NormalRTRef);
        const auto MaterialId = ViewGraph.Import(MaterialIdRTRef);
        const auto EntityId   = ViewGraph.Import(EntityIdRTRef);
        const auto Depth      = ViewGraph.Import(DepthRTRef);

        // Selection filter outputs live at view scope: the mask pass consumes
        // them and is registered outside the instance block.
        RGStorageBufferHandle SelectedInstances = {};
        RGStorageBufferHandle SelectedIndirect  = {};

        if (HasInstances) {
            const auto InstanceDataSize = DrawData.Instances.size() * sizeof(InstanceRecord::GpuData);
            const auto IndirectDataSize = DrawData.IndirectCommands.size() * sizeof(RasterIndirectCommand);

            const auto ViewInstances = ViewGraph.CreateShaderStorageBuffer(
                "ViewInstances",
                RGShaderStorageBufferDesc{
                    .SizeBytes = InstanceDataSize,
                    .Stride = static_cast<Uint32>(sizeof(InstanceRecord::GpuData)),
                    .Usage = RHITransientBufferUsage::ShaderRead,
                });
            const auto ViewIndirect = ViewGraph.CreateShaderStorageBuffer(
                "ViewIndirect",
                RGShaderStorageBufferDesc{
                    .SizeBytes = IndirectDataSize,
                    .Stride = static_cast<Uint32>(sizeof(RasterIndirectCommand)),
                    .Usage = RHITransientBufferUsage::ShaderRead | RHITransientBufferUsage::IndirectCommandRead,
                });
            const auto ViewCounter = ViewGraph.CreateShaderStorageBuffer(
                "ViewCounter",
                RGShaderStorageBufferDesc{
                    .SizeBytes = sizeof(Uint32),
                    .Usage = RHITransientBufferUsage::ShaderRead,
                });

            ViewGraph.AddPass<CullingPass>(CullingPass::Parameter{
                .ViewInstances  = RGStorageBufferUAV{.Buffer = ViewInstances},
                .ViewIndirect   = RGStorageBufferUAV{.Buffer = ViewIndirect},
                .ViewCounter    = RGStorageBufferUAV{.Buffer = ViewCounter},
                .SceneInstances = RGStorageBufferSRV{.Buffer = SceneInstances},
                .SceneIndirect  = RGStorageBufferSRV{.Buffer = SceneIndirect},
                .GeometryTable  = RGStorageBufferSRV{.Buffer = GeometryTable},
                .InstanceCount  = static_cast<Uint32>(DrawData.Instances.size()),
                .CommandCount   = static_cast<Uint32>(DrawData.IndirectCommands.size()),
            });

            if (EditorView && Editor.SelectedEntity) {
                SelectedInstances = ViewGraph.CreateShaderStorageBuffer(
                    "SelectedInstances",
                    RGShaderStorageBufferDesc{
                        .SizeBytes = InstanceDataSize,
                        .Stride = static_cast<Uint32>(sizeof(InstanceRecord::GpuData)),
                        .Usage = RHITransientBufferUsage::ShaderRead,
                    });
                SelectedIndirect = ViewGraph.CreateShaderStorageBuffer(
                    "SelectedIndirect",
                    RGShaderStorageBufferDesc{
                        .SizeBytes = IndirectDataSize,
                        .Stride = static_cast<Uint32>(sizeof(RasterIndirectCommand)),
                        .Usage = RHITransientBufferUsage::ShaderRead | RHITransientBufferUsage::IndirectCommandRead,
                    });
                // The filter shader accumulates per-command instance counts via
                // InterlockedAdd, so the counters must start at zero every frame.
                const std::vector<std::byte> ZeroCounters(
                    DrawData.IndirectCommands.size() * sizeof(Uint32), std::byte{0});
                const auto SelectedCounters = ViewGraph.CreateShaderStorageBuffer(
                    "SelectedCounters",
                    RGShaderStorageBufferDesc{
                        .SizeBytes   = ZeroCounters.size(),
                        .Usage       = RHITransientBufferUsage::ShaderRead,
                        .InitialData = ZeroCounters,
                    });

                ViewGraph.AddPass<SelectionFilterPass>(SelectionFilterPass::Parameter{
                    .ViewInstances     = RGStorageBufferSRV{.Buffer = ViewInstances},
                    .ViewIndirect      = RGStorageBufferSRV{.Buffer = ViewIndirect},
                    .SelectedInstances = RGStorageBufferUAV{.Buffer = SelectedInstances},
                    .SelectedIndirect  = RGStorageBufferUAV{.Buffer = SelectedIndirect},
                    .SelectedCounters  = RGStorageBufferUAV{.Buffer = SelectedCounters},
                    .CommandCount      = static_cast<Uint32>(DrawData.IndirectCommands.size()),
                    .SelectedEntityId  = static_cast<Uint32>(entt::to_integral(*Editor.SelectedEntity)),
                });
            }

            ViewGraph.AddPass<GeometryPass>(GeometryPass::Parameter{
                .Albedo         = RGColorRT{.Texture = Albedo},
                .Normal         = RGColorRT{.Texture = Normal},
                .MaterialId     = RGColorRT{.Texture = MaterialId},
                .EntityId       = RGColorRT{.Texture = EntityId},
                .Depth          = RGDepthRT{.Texture = Depth},
                .InstanceBuffer = RGStorageBufferSRV{.Buffer = ViewInstances},
                .IndirectBuffer = RGIndirectBuffer{.Buffer = ViewIndirect},
                .GeometryBuffer = RGStorageBufferSRV{.Buffer = GeometryTable},
                .MaterialBuffer = RGStorageBufferSRV{.Buffer = MaterialTable},
                .FrameBuffer    = {.Buffer = FrameCB},
                .ViewBuffer     = {.Buffer = ViewCB},
                .LinearSampler      = SamplerLinearRef,
                .AnisotropicSampler = SamplerAnisoRef,
                .Textures           = Scene.Textures,
                .DrawCount          = static_cast<Uint32>(DrawData.IndirectCommands.size()),
            });
        }

        if (Editor.IsHovering && Editor.ReadbackTarget) {
            const auto Readback = ViewGraph.Import(Editor.ReadbackTarget);
            ViewGraph.AddPass<EntityPickingPass>(EntityPickingPass::Parameter{
                .EntityId = RGCopySrc{.Texture = EntityId},
                .Readback = RGCopyDst{.Buffer = Readback},
                .Pixel    = Editor.HoverPixel,
            });
        }

        const auto SceneColor = ViewGraph.Import(SceneColorRTRef);
        // One pass, runtime frame config: the write is the terminal present
        // write without the outline overblend, or a plain color write that
        // SelectionOutline's load RMW blends over and presents.
        ViewGraph.AddPass<DeferredLightingPass>(DeferredLightingPass::Parameter{
            .SceneColor     = RGColorRT{.Texture = SceneColor, .Present = !WantOutline},
            .Albedo         = RGTextureSRV{.Texture = Albedo},
            .Normal         = RGTextureSRV{.Texture = Normal},
            .MaterialId     = RGTextureSRV{.Texture = MaterialId},
            .EntityId       = RGTextureSRV{.Texture = EntityId},
            .Depth          = RGTextureSRV{.Texture = Depth},
            .MaterialBuffer = RGStorageBufferSRV{.Buffer = MaterialTable},
            .FrameBuffer    = {.Buffer = DeferredFrameCB},
            .ViewBuffer     = {.Buffer = DeferredViewCB},
            .LightBuffer    = RGStorageBufferSRV{.Buffer = LightTable},
            .LinearSampler      = SamplerLinearRef,
            .AnisotropicSampler = SamplerAnisoRef,
            .Textures           = Scene.Textures,
        });

        if (WantOutline) {
            const auto& MaskRT = EditorView->SelectionMask;
            const auto Mask = ViewGraph.Import(MaskRT);
            ViewGraph.AddPass<SelectionMaskPass>(SelectionMaskPass::Parameter{
                .Mask              = RGColorRT{.Texture = Mask},
                .SelectedInstances = RGStorageBufferSRV{.Buffer = SelectedInstances},
                .SelectedIndirect  = RGIndirectBuffer{.Buffer = SelectedIndirect},
                .GeometryTable     = RGStorageBufferSRV{.Buffer = GeometryTable},
                .MaterialTable     = RGStorageBufferSRV{.Buffer = MaterialTable},
                .FrameBuffer       = {.Buffer = FrameCB},
                .ViewBuffer        = {.Buffer = ViewCB},
                .LinearSampler     = SamplerLinearRef,
                .Textures          = Scene.Textures,
                .DrawCount         = static_cast<Uint32>(DrawData.IndirectCommands.size()),
            });
            ViewGraph.AddPass<SelectionOutlinePass>(SelectionOutlinePass::Parameter{
                .SceneColor    = RGColorRT{.Texture = SceneColor, .Load = true},
                .Mask          = RGTextureSRV{.Texture = Mask},
                .LinearSampler = SamplerLinearRef,
                .ViewportWidth  = static_cast<Float32>(View.GetWidth()),
                .ViewportHeight = static_cast<Float32>(View.GetHeight()),
            });
        }

        // Compile/splice anchor: the per-view graph orders, prunes, realizes,
        // and constructs the feature-gated chain in one call. Present rides
        // RGColorRT{.Present} or the outline RMW + PresentOutput carrier.
        auto ViewGraphPasses = ViewGraph.Compile();
        if (!ViewGraphPasses)
            return std::unexpected(ViewGraphPasses.error().Append("Raster view render-graph compile failed"));
        CmdList.Passes.insert(CmdList.Passes.end(),
                              std::make_move_iterator(ViewGraphPasses->Passes.begin()),
                              std::make_move_iterator(ViewGraphPasses->Passes.end()));
        return {};
    }

    [[nodiscard]] static auto BuildFrameConstants(Float32 Time, Float32 ExposureEV100, Uint32 LightCount)
        -> RendererFrameConstants {
        return RendererFrameConstants{.Time = Time, .ExposureEV100 = ExposureEV100, .LightCount = LightCount};
    }

    RHIRef<RHISampler>                    m_SamplerLinear            = nullptr;
    RHIRef<RHISampler>                    m_SamplerAniso             = nullptr;
};

RendererFactory::AutoRegistrar<RasterRenderer> RegRasterRenderer{"Raster"};

} // namespace SoulEngine
