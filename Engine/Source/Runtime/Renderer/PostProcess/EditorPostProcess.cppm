export module Renderer:PostProcess.EditorPostProcess;

import Core;
import RHI;
import Resource;
import Scene;

export import std;

export namespace SoulEngine {

/// @brief Push-constant layout for the editor EntityId outline pass.
struct EditorSelectionConstants {
    Uint32 SelectedPixelX = 0;
    Uint32 SelectedPixelY = 0;
    Uint32 Enabled        = 0;
    Uint32 Padding        = 0;
};

/// @brief Queue preparation of the editor EntityId outline pipeline.
[[nodiscard]] auto RequestEditorSelectionPipeline() -> std::expected<RHIRef<RHIGraphicsPipeline>, ErrorMessage> {
    const auto ShaderPath = ConfigManager::Get().EngineShadersDirPath() / "EditorSelectionOutline.slang";
    auto Result = RequestGraphicsPipeline(
        "EditorSelectionPass",
        GraphicsPipelineRequest{
            .VertEntry    = {.SourcePath = ShaderPath, .EntryPoint = "vertMain"},
            .FragEntry    = {.SourcePath = ShaderPath, .EntryPoint = "fragMain"},
            .DepthStencil = {.DepthTestEnable = false, .DepthWriteEnable = false},
            .ColorFormats = {RHIFormat::B8G8R8A8_UNORM},
    });
    if (!Result)
        return std::unexpected(Result.error());
    return *Result;
}

/// @brief Construct the editor selection outline pass for one render view.
class EditorSelectionPass final : public IRHIGraphicsPass {
  public:
    explicit EditorSelectionPass(RHIRef<RHIGraphicsPipeline> Pipeline)
        : IRHIGraphicsPass(std::move(Pipeline)) {}

    auto SetInput(const CameraViewRecord& View, const RenderPixelCoordinate& Pixel) -> void {
        m_EntityIdRT = View.Targets.GBuffer.EntityIdRT;
        m_SceneColorRT = View.Targets.SceneColorRT;
        m_Pixel = Pixel;
    }

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        m_Attachments = {};
        if (!GetShaderBindingSet(m_Pipeline) || !m_EntityIdRT || !m_SceneColorRT)
            return std::unexpected(ErrorMessage("Editor selection post-process resources are not ready"));

        m_Attachments = RHIGraphicsAttachments{
            .ColorAttachments =
                {
                    {.TextureRef = m_SceneColorRT, .Clear = false},
                },
        };
        SetViewport(0.0f,
                    0.0f,
                    static_cast<Float32>(m_SceneColorRT->GetWidth()),
                    static_cast<Float32>(m_SceneColorRT->GetHeight()));
        SetScissorRect(0, 0, m_SceneColorRT->GetWidth(), m_SceneColorRT->GetHeight());

        if (auto R = BindResource("g_editorSelection.entityId", m_EntityIdRT, true); !R)
            return std::unexpected(R.error().Append("Editor selection entity ID binding failed"));
        const EditorSelectionConstants Constants{
            .SelectedPixelX = m_Pixel.X,
            .SelectedPixelY = m_Pixel.Y,
            .Enabled        = 1,
        };
        if (auto R = PushConstants(0, Constants); !R)
            return std::unexpected(R.error().Append("Editor selection push constant binding failed"));
        if (auto R = Commit(); !R)
            return std::unexpected(R.error().Append("Editor selection binding commit failed"));
        Draw();
        return {};
    }

  private:
    RHIRef<RHIRenderTarget> m_EntityIdRT = nullptr;
    RHIRef<RHIRenderTarget> m_SceneColorRT = nullptr;
    RenderPixelCoordinate  m_Pixel = {};
};

/// @brief Construct the editor selection outline pass for one render view.
[[nodiscard]] auto BuildEditorSelectionPass(const CameraViewRecord&            View,
                                            const RenderPixelCoordinate&       Pixel,
                                            const RHIRef<RHIGraphicsPipeline>& Pipeline)
    -> std::expected<UPtr<IRHIPass>, ErrorMessage> {
    const auto& EntityIdRTRef = View.Targets.GBuffer.EntityIdRT;
    const auto& SceneColorRef = View.Targets.SceneColorRT;
    if (!EntityIdRTRef || !SceneColorRef || Pipeline.GetState() == RHIRefState::Unknown)
        return std::unexpected(ErrorMessage("Editor selection post-process resources are not ready"));

    auto Pass = std::make_unique<EditorSelectionPass>(Pipeline);
    Pass->SetInput(View, Pixel);
    return Pass;
}

} // namespace SoulEngine
