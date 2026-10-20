module;

export module Renderer:Common.BlitToSwapchainPass;

import RenderGraph;
import Core;
import RHI;

import std;

namespace SoulEngine {

/// @brief Terminal blit of a view's final output into the swapchain.
///
/// The class IS the graph's TPass: a typed NON-pipeline transfer pass (no BuildPipelineRequest), constructed
/// with its Parameter. This is the only present path: the backend resolves the acquired swapchain image,
/// which stays backend-private. NeverPrune — the pass writes no graph resource, so without the marker pruning
/// would drop the frame's present.
export class BlitToSwapchainPass final : public IRHITransferPass {
  public:
    static constexpr StringView Name       = "BlitToSwapchainPass";
    static constexpr bool       NeverPrune = true;
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    struct Parameter {
        RGTextureView<RHITextureUsage::TransferSrc, RHIAccess::Read> Source;
        // Destination rectangle on the swapchain, in physical pixels (the camera's Viewport).
        Uint32                                                       DstX      = 0;
        Uint32                                                       DstY      = 0;
        Uint32                                                       DstWidth  = 0;
        Uint32                                                       DstHeight = 0;
    };

    BlitToSwapchainPass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {}

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        if (!m_Parameter->Source.Resolved)
            return std::unexpected(ErrorMessage("Blit-to-swapchain source is not ready"));
        // step 1: blit the whole source target into the destination rectangle
        BlitToSwapchain(m_Parameter->Source.Resolved,
                        m_Parameter->DstX,
                        m_Parameter->DstY,
                        m_Parameter->DstWidth,
                        m_Parameter->DstHeight);
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

} // namespace SoulEngine
