export module Renderer:Raster.RasterRenderer;

import Core;
import EditorTypes;
import RHI;
import Scene;

import :Common;
import :Interface;

import std;

namespace SoulEngine {

export struct alignas(16) RasterIndirectCommand {
    Uint32 VertexCount   = 0;
    Uint32 InstanceCount = 1;
    Uint32 FirstVertex   = 0;
    Uint32 FirstInstance = 0;
};
static_assert(sizeof(RasterIndirectCommand) == sizeof(Uint32) * 4);

/// @brief Group the canonically sorted instance table into one indirect draw
/// per geometry. The instance table must already be sorted by GeometryID (SceneGpuData's canonical order).
export [[nodiscard]] auto BuildRasterIndirectCommands(const SceneGpuData& DrawData)
    -> std::vector<RasterIndirectCommand>;

/// @brief Single-material metallic-roughness forward renderer.
export class RasterRenderer final : public IRenderer {
  public:
    RasterRenderer() = default;
    ~RasterRenderer() override;

    [[nodiscard]] auto OnAttach() -> std::expected<void, ErrorMessage> override;

    auto OnDetach() -> void override;

    [[nodiscard]] auto Render(const GameSnapshot& Scene, const EditorSnapshot& Editor)
        -> std::expected<RenderResult, ErrorMessage> override;

  private:
    /// @brief Upload-gate the frame's instances: queue lazy uploads on first
    /// encounter, then keep only GPU-ready instances for this frame's tables.
    [[nodiscard]] auto FilterInstances(std::span<const InstanceRecord> SourceInstances) -> std::vector<InstanceRecord>;

    [[nodiscard]] auto RenderView(RenderPassList&                        CmdList,
                                  StringView                             ViewName,
                                  const SceneGpuData&                    DrawData,
                                  std::span<const RasterIndirectCommand> IndirectCommands,
                                  const CameraViewRecord&                View,
                                  const GameSnapshot&                    Scene,
                                  const EditorSnapshot&                  Editor,
                                  const EditorViewRecord* EditorView) -> std::expected<void, ErrorMessage>;

    [[nodiscard]] static auto BuildFrameConstants(Float32 Time, Float32 ExposureEV100, Uint32 LightCount)
        -> RendererFrameConstants;

    RHIRef<RHISampler> m_SamplerLinear = nullptr;
    RHIRef<RHISampler> m_SamplerAniso  = nullptr;
    GeometryUploader   m_Uploader      = {};
};

} // namespace SoulEngine
