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
[[nodiscard]] auto RequestEditorSelectionPipeline()
    -> std::expected<RHIRef<RHIGraphicsPipeline>, ErrorMessage> {
    const auto ShaderPath = ConfigManager::Get().EngineShadersDirPath() / "EditorSelectionOutline.slang";
    return RequestGraphicsPipeline(GraphicsPipelineRequest{
        .VertEntry    = {.SourcePath = ShaderPath, .EntryPoint = "vertMain"},
        .FragEntry    = {.SourcePath = ShaderPath, .EntryPoint = "fragMain"},
        .DepthStencil = {.DepthTestEnable = false, .DepthWriteEnable = false},
        .ColorFormats = {RHIFormat::B8G8R8A8_UNORM},
    });
}

/// @brief Construct the editor selection outline pass for one render view.
[[nodiscard]] auto BuildEditorSelectionPass(const RenderViewSnapshot& View,
                                            const RenderPixelCoordinate& Pixel,
                                            const RHIRef<RHIGraphicsPipeline>& Pipeline)
    -> std::expected<RHIPass, ErrorMessage> {
    const auto& EntityIdRTRef   = View.Targets.GBuffer.EntityIdRT;
    const auto& SceneColorRef   = View.Targets.SceneColorRT;
    if (!EntityIdRTRef || !SceneColorRef || !Pipeline)
        return std::unexpected(ErrorMessage("Editor selection post-process resources are not ready"));

    RHIPass Pass{
        .Desc = RHIRenderingDesc{
            .ColorAttachments = {
                {.TextureRef = SceneColorRef, .Clear = false},
            },
        },
    };
    Pass.SetFullViewport();
    Pass.SetFullScissorRect();
    Pass.SetGraphicsPipeline(Pipeline);

    auto Parameters = RHIShaderParameters::Create(*Pipeline);
    if (auto R = Parameters.SetSampledRenderTarget("g_editorSelection.entityId", EntityIdRTRef); !R)
        return std::unexpected(R.error().Append("Editor selection entity ID binding failed"));

    const EditorSelectionConstants Constants{
        .SelectedPixelX = Pixel.X,
        .SelectedPixelY = Pixel.Y,
        .Enabled        = 1,
    };
    Pass.PushConstants(Pipeline, 0, &Constants, sizeof(Constants));
    Pass.BindShaderParameters(
        Pipeline,
        std::move(Parameters),
        RHIShaderParameterResources{
            .RenderTargets = {SceneColorRef, EntityIdRTRef},
        });
    Pass.Draw(Pipeline);
    return Pass;
}

} // namespace SoulEngine
