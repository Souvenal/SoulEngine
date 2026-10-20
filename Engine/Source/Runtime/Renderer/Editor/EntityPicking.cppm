export module Renderer:EditorPasses.EntityPicking;

import Core;
import EditorTypes;
import RHI;
import RenderGraph;
import Scene;

import std;

namespace SoulEngine {

/// @brief Copy-only transfer pass that reads back the GBuffer EntityId texel at the requested pixel into a
/// readback.
///
/// The class IS the graph's TPass: a typed NON-pipeline transfer pass (no BuildPipelineRequest → no
/// Ensure/Pending-prune participation), constructed with its Parameter. The readback is a passthrough field —
/// a consumer object, not a graph resource; the graph sees only the source texture's TransferSrc read, which
/// carries the ordering and layout transition. NeverPrune stands in for the graph-invisible ring write.
export class EntityPickingPass final : public IRHITransferPass {
  public:
    static constexpr StringView Name       = "EntityPickingPass";
    static constexpr bool       NeverPrune = true;
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    struct Parameter {
        RGTextureView<RHITextureUsage::TransferSrc, RHIAccess::Read> EntityId;
        RHIRef<RHIReadback>                                          Readback = nullptr;
        PixelCoordinate                                              Pixel    = {};
    };

    EntityPickingPass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {}

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        if (!m_Parameter->EntityId.Resolved || !m_Parameter->Readback)
            return std::unexpected(ErrorMessage("Entity picking resources are not ready"));
        ReadbackTexture(
            m_Parameter->EntityId.Resolved, m_Parameter->Pixel.X, m_Parameter->Pixel.Y, m_Parameter->Readback);
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

} // namespace SoulEngine
