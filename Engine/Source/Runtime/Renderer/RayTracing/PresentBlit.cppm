module;

export module Renderer:RayTracing.PresentBlit;

import Core;
import RHI;
import RenderGraph;

export import std;

export namespace SoulEngine {

/// @brief Fullscreen copy of the ray-tracing output into the view's
/// SceneColorRT, carrying the frame Present.
///
/// The trace pass writes its result as a storage image; the graph's present
/// path requires a graphics pass with a `.Present` color attachment, so this
/// pass performs the final texel-exact copy.
class PresentBlitPass final : public IRHIGraphicsPass {
  public:
    static constexpr StringView Name = "PresentBlitPass";

    /// Static pipeline descriptor for PipelineRegistry::Register<PresentBlitPass>().
    [[nodiscard]] static auto BuildPipelineRequest() -> GraphicsPipelineRequest {
        const auto ShaderPath = ConfigManager::Get().EngineShadersDirPath() / "RayTracing" / "PresentBlit.slang";
        return GraphicsPipelineRequest{
            .VertEntry    = {.SourcePath = ShaderPath, .EntryPoint = "vertMain"},
            .FragEntry    = {.SourcePath = ShaderPath, .EntryPoint = "fragMain"},
            .ColorFormats = {RHIFormat::B8G8R8A8_UNORM},
        };
    }

    struct Parameter {
        RGTextureSRV Source     = {};
        RGColorRT    SceneColor = {};
    };

    PresentBlitPass(Parameter In, RHIRef<RHIGraphicsPipeline> Pipeline)
        : IRHIGraphicsPass(String(Name), std::move(Pipeline)), m_Parameter(std::move(In)) {}

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        m_Attachments = {};

        if (!m_Parameter.Source.Ref || !m_Parameter.SceneColor.Ref)
            return std::unexpected(ErrorMessage("Present blit pass resources are not ready"));

        m_Attachments = RHIGraphicsAttachments{
            .ColorAttachments = {
                {.TextureRef = m_Parameter.SceneColor.Ref,
                 .ClearValue = std::array<Float32, 4>{0.0f, 0.0f, 0.0f, 0.0f},
                 .Clear      = false},
            },
        };
        SetViewport(0.0f,
                    0.0f,
                    static_cast<Float32>(m_Parameter.SceneColor.Ref->GetWidth()),
                    static_cast<Float32>(m_Parameter.SceneColor.Ref->GetHeight()));
        SetScissorRect(0, 0, m_Parameter.SceneColor.Ref->GetWidth(), m_Parameter.SceneColor.Ref->GetHeight());

        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_blit.sourceTexture", m_Parameter.Source.Ref, true},
            }); !R)
            return std::unexpected(R.error());
        Draw();
        return {};
    }

  private:
    Parameter m_Parameter = {};
};

} // namespace SoulEngine
