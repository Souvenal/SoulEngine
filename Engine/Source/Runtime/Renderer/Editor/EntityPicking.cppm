export module Renderer:EditorPasses.EntityPicking;

import Core;
import EditorTypes;
import RHI;
import Scene;

export import std;

export namespace SoulEngine {

/// @brief Copy-only compute pass that reads back the GBuffer EntityId texel
/// at the requested pixel into a readback buffer.
class EntityPickingPass final : public IRHITransferPass {
  public:
    EntityPickingPass() : IRHITransferPass("EntityPickingPass") {}

    auto SetInput(RHIRef<RHIRenderTarget> EntityId, PixelCoordinate Pixel, RHIRef<RHIReadbackBuffer> Target) -> void {
        m_EntityId = std::move(EntityId);
        m_Pixel    = Pixel;
        m_Target   = std::move(Target);
    }

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        if (!m_EntityId || !m_Target)
            return std::unexpected(ErrorMessage("Entity picking resources are not ready"));
        CopyTextureToBuffer(m_EntityId, m_Pixel.X, m_Pixel.Y, m_Target);
        return {};
    }

  private:
    RHIRef<RHIRenderTarget> m_EntityId = nullptr;
    PixelCoordinate         m_Pixel    = {};
    RHIRef<RHIReadbackBuffer> m_Target = nullptr;
};

/// @brief Build the entity picking pass for one frame's picking request.
[[nodiscard]] auto BuildEntityPickingPass(RHIRef<RHIRenderTarget> EntityId, PixelCoordinate Pixel, RHIRef<RHIReadbackBuffer> Target)
    -> std::expected<UPtr<IRHIPass>, ErrorMessage> {
    if (!EntityId)
        return std::unexpected(ErrorMessage("Entity picking requires a ready EntityId target"));
    auto Pass = std::make_unique<EntityPickingPass>();
    Pass->SetInput(std::move(EntityId), Pixel, std::move(Target));
    return Pass;
};

} // namespace SoulEngine
