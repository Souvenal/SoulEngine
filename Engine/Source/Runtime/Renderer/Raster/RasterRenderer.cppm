export module Renderer.Raster.RasterRenderer;

import Core;
import EditorTypes;
import RHI;
import Scene;

import Renderer.Common;
import Renderer.Interface;

import std;

namespace SoulEngine {

/// @brief Strongly typed 20-byte indexed-indirect command.
export struct RasterIndirectCommand {
    Uint32 IndexCount    = 0;
    Uint32 InstanceCount = 1;
    Uint32 FirstIndex    = 0;
    Int32  VertexOffset  = 0;
    Uint32 FirstInstance = 0;
};
static_assert(sizeof(RasterIndirectCommand) == sizeof(Uint32) * 5);

/// @brief CPU-side geometry identity and indexed-indirect command for one stable slot.
export struct RasterDrawGroup {
    Uint32                GeometryID = 0;
    RasterIndirectCommand Command    = {};
};

/// @brief Group canonically sorted instances into stable geometry draw slots.
export [[nodiscard]] auto BuildRasterDrawGroups(const SceneGpuData& DrawData) -> std::vector<RasterDrawGroup>;

/// @brief Copy draw-group commands into their GPU-upload layout.
export [[nodiscard]] auto BuildRasterIndirectCommands(std::span<const RasterDrawGroup> DrawGroups)
    -> std::vector<RasterIndirectCommand>;

/// @brief Build indexed draw groups and flatten their commands.
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
                                  std::span<const RasterDrawGroup>       DrawGroups,
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
