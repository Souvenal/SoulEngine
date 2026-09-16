module;

export module RenderGraph:BlitToSwapchain;

import :Types;
import Core;
import RHI;

export import std;

export namespace SoulEngine {

/// @brief Terminal blit of a view's final output into the swapchain.
///
/// The class IS the graph's TPass: a typed NON-pipeline transfer pass (no
/// BuildPipelineRequest), constructed with its Parameter. This is the only
/// present path (ADR 05): the backend resolves the acquired swapchain image,
/// which stays backend-private. NeverPrune — the pass writes no graph
/// resource, so without the marker pruning would drop the frame's present.
class BlitToSwapchainPass final : public IRHITransferPass {
  public:
    static constexpr StringView Name       = "BlitToSwapchainPass";
    static constexpr bool       NeverPrune = true;
    [[nodiscard]] auto GetName() const noexcept -> StringView override { return Name; }

    struct Parameter {
        RGCopySrc Source    = {};
        /// Destination rectangle on the swapchain, in physical pixels
        /// (the camera's Viewport).
        Uint32    DstX      = 0;
        Uint32    DstY      = 0;
        Uint32    DstWidth  = 0;
        Uint32    DstHeight = 0;
    };

    BlitToSwapchainPass(Parameter In) : m_Parameter(std::move(In)) {}

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        if (!m_Parameter.Source.Ref)
            return std::unexpected(ErrorMessage("Blit-to-swapchain source is not ready"));
        // step 1: blit the whole source target into the destination rectangle
        BlitToSwapchain(m_Parameter.Source.Ref,
                        m_Parameter.DstX,
                        m_Parameter.DstY,
                        m_Parameter.DstWidth,
                        m_Parameter.DstHeight);
        return {};
    }

  private:
    Parameter m_Parameter = {};
};

} // namespace SoulEngine
