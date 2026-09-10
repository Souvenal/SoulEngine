export module Renderer:EditorPasses.EntityPicking;

import Core;
import EditorTypes;
import RHI;
import RenderGraph;
import Scene;

export import std;

export namespace SoulEngine {

/// @brief Copy-only transfer pass that reads back the GBuffer EntityId texel
/// at the requested pixel into a readback buffer.
///
/// The class IS the graph's TPass: a typed NON-pipeline transfer pass (no
/// BuildPipelineRequest → no Ensure/Pending-prune participation), constructed
/// with its Parameter. The RGCopyDst view carries the implicit side effect.
class EntityPickingPass final : public IRHITransferPass {
  public:
    static constexpr StringView Name = "EntityPickingPass";

    struct Parameter {
        RGCopySrc       EntityId = {};
        RGCopyDst       Readback = {};
        PixelCoordinate Pixel    = {};
    };

    EntityPickingPass(Parameter In) : IRHITransferPass(String(Name)), m_Parameter(std::move(In)) {}

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        if (!m_Parameter.EntityId.Ref || !m_Parameter.Readback.Ref)
            return std::unexpected(ErrorMessage("Entity picking resources are not ready"));
        CopyTextureToBuffer(m_Parameter.EntityId.Ref, m_Parameter.Pixel.X, m_Parameter.Pixel.Y,
                            m_Parameter.Readback.Ref);
        return {};
    }

  private:
    Parameter m_Parameter = {};
};

} // namespace SoulEngine
