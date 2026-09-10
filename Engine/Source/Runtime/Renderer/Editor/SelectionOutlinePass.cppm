module;

export module Renderer:EditorPasses.SelectionOutlinePass;

import Core;
import RHI;
import RenderGraph;

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

/// @brief Blends the outline color over the lit SceneColor through the
/// selection mask and carries the frame Present.
///
/// The class IS the graph's TPass. SceneColor is the only legal fused RMW —
/// a loadOp=Load color attachment declared via `RGColorRT{.Load = true}`.
class SelectionOutlinePass final : public IRHIGraphicsPass {
  public:
    static constexpr StringView Name = "SelectionOutlinePass";

    /// Static pipeline descriptor for PipelineRegistry::Register<SelectionOutlinePass>().
    [[nodiscard]] static auto BuildPipelineRequest() -> GraphicsPipelineRequest {
        const auto ShaderPath = ConfigManager::Get().EngineShadersDirPath() / "Editor" / "SelectionOutline.slang";
        return GraphicsPipelineRequest{
            .VertEntry = {.SourcePath = ShaderPath, .EntryPoint = "vertMain"},
            .FragEntry = {.SourcePath = ShaderPath, .EntryPoint = "fragMain"},
            .Blend     =
                RHIBlendState{.Attachments = {RHIBlendAttachment{
                    .BlendEnable         = true,
                    .SrcColorBlendFactor = RHIBlendFactor::SrcAlpha,
                    .DstColorBlendFactor = RHIBlendFactor::OneMinusSrcAlpha,
                    .ColorBlendOp        = RHIBlendOp::Add,
                    .SrcAlphaBlendFactor = RHIBlendFactor::One,
                    .DstAlphaBlendFactor = RHIBlendFactor::Zero,
                    .AlphaBlendOp        = RHIBlendOp::Add,
                }}},
            .ColorFormats = {RHIFormat::B8G8R8A8_UNORM},
        };
    }

    /// Present carrier: the graph marks this pass as the frame's present
    /// output (its SceneColor attachment is a Load RMW, so no view bit says
    /// so — the marker does).
    static constexpr bool PresentOutput = true;

    struct Parameter {
        /// Fused read-modify-write attachment: one internal RmwAttachment
        /// access, never a Read+Write pair.
        RGColorRT    SceneColor = {.Load = true};
        RGTextureSRV Mask       = {};
        RHIRef<RHISampler> LinearSampler = nullptr;
        Float32 ViewportWidth  = 0.0f;
        Float32 ViewportHeight = 0.0f;
    };

    SelectionOutlinePass(Parameter In, RHIRef<RHIGraphicsPipeline> Pipeline)
        : IRHIGraphicsPass(String(Name), std::move(Pipeline)), m_Parameter(std::move(In)) {}

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        m_Attachments = {};

        if (!m_Parameter.SceneColor.Ref || !m_Parameter.Mask.Ref ||
            !m_Parameter.LinearSampler)
            return std::unexpected(ErrorMessage("Selection outline pass resources are not ready"));

        m_Attachments = RHIGraphicsAttachments{
            .ColorAttachments = {
                {.TextureRef = m_Parameter.SceneColor.Ref,
                 .ClearValue = std::array<Float32, 4>{0.0f, 0.0f, 0.0f, 0.0f},
                 .Clear = false},
            },
        };
        SetViewport(0.0f,
                    0.0f,
                    static_cast<Float32>(m_Parameter.SceneColor.Ref->GetWidth()),
                    static_cast<Float32>(m_Parameter.SceneColor.Ref->GetHeight()));
        SetScissorRect(0, 0, m_Parameter.SceneColor.Ref->GetWidth(), m_Parameter.SceneColor.Ref->GetHeight());

        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_outline.maskTexture", m_Parameter.Mask.Ref, true},
                RHIShaderBindingRequest{"g_outline.sampler", m_Parameter.LinearSampler, true},
            }); !R)
            return std::unexpected(R.error());
        if (auto R = PushConstants(0,
                                   SelectionOutlinePushConstants{
                                       .ViewportWidth = m_Parameter.ViewportWidth,
                                       .ViewportHeight = m_Parameter.ViewportHeight,
                                   });
            !R)
            return std::unexpected(R.error());
        Draw();
        return {};
    }

  private:
    Parameter m_Parameter = {};
};

} // namespace SoulEngine
