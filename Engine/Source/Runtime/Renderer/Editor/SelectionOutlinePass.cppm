module;

export module Renderer:EditorPasses.SelectionOutlinePass;

import Core;
import RHI;

export import std;

namespace SoulEngine {
namespace {

struct SelectionOutlinePushConstants {
    Float32 ViewportWidth;
    Float32 ViewportHeight;
};

static_assert(sizeof(SelectionOutlinePushConstants) == 8);

} // namespace
} // namespace SoulEngine

export namespace SoulEngine {

struct SelectionOutlinePassInput {
    RHIRef<RHIRenderTarget>       SceneColor = nullptr;
    RHIRef<RHIRenderTarget>       Mask = nullptr;
    RHIRef<RHISampler>            LinearSampler = nullptr;
    float                         ViewportWidth = 0.0f;
    float                         ViewportHeight = 0.0f;
};

class SelectionOutlinePass final : public IRHIGraphicsPass {
  public:
    explicit SelectionOutlinePass(RHIRef<RHIGraphicsPipeline> Pipeline)
        : IRHIGraphicsPass("SelectionOutlinePass", std::move(Pipeline)) {}

    auto SetInput(SelectionOutlinePassInput Input) -> void {
        m_Input = std::move(Input);
    }

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        m_Attachments = {};
        auto Input = std::move(m_Input);

        if (!GetShaderBindingSet() || !Input.SceneColor || !Input.Mask || !Input.LinearSampler)
            return std::unexpected(ErrorMessage("Selection outline pass resources are not ready"));

        m_Attachments = RHIGraphicsAttachments{
            .ColorAttachments = {
                {.TextureRef = Input.SceneColor,
                 .ClearValue = std::array<Float32, 4>{0.0f, 0.0f, 0.0f, 0.0f},
                 .Clear = false},
            },
        };
        SetViewport(0.0f,
                    0.0f,
                    static_cast<Float32>(Input.SceneColor->GetWidth()),
                    static_cast<Float32>(Input.SceneColor->GetHeight()));
        SetScissorRect(0, 0, Input.SceneColor->GetWidth(), Input.SceneColor->GetHeight());

        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_outline.maskTexture", std::move(Input.Mask), true},
                RHIShaderBindingRequest{"g_outline.sampler", std::move(Input.LinearSampler), true},
            }); !R)
            return std::unexpected(R.error());
        if (auto R = PushConstants(0,
                                   SelectionOutlinePushConstants{
                                       .ViewportWidth = Input.ViewportWidth,
                                       .ViewportHeight = Input.ViewportHeight,
                                   });
            !R)
            return std::unexpected(R.error());
        Draw();
        return {};
    }

  private:
    SelectionOutlinePassInput m_Input = {};
};

} // namespace SoulEngine
