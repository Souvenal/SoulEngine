module;

export module Renderer:EditorPasses.SelectionOutlinePass;

import Core;
import RHI;
import RenderGraph;

import std;
import ShaderCompiler;

namespace SoulEngine {

// Module linkage (plain namespace scope), not an anonymous namespace: named by the exported code below — see
// Core:CVar's linkage note.

struct SelectionOutlinePushConstants {
    Float32 ViewportWidth;
    Float32 ViewportHeight;
};

static_assert(sizeof(SelectionOutlinePushConstants) == 8);

} // namespace SoulEngine

export namespace SoulEngine {

/// @brief Blends the outline color over the lit SceneColor through the
/// selection mask.
///
/// The class IS the graph's TPass. SceneColor is the only legal fused RMW — a loadOp=Load color attachment
/// declared as Read|Write access on the view. Present is the explicit BlitToSwapchainPass downstream.
class SelectionOutlinePass final : public IRHIGraphicsPass {
  public:
    static constexpr StringView Name = "SelectionOutlinePass";
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    /// Static pipeline descriptor for PipelineRegistry::Register<SelectionOutlinePass>().
    [[nodiscard]] static auto BuildPipelineRequest() -> GraphicsPipelineRequest {
        const auto ShaderPath = EnginePaths::Get().EngineShadersDirPath() / "Editor" / "SelectionOutline.slang";
        return GraphicsPipelineRequest{
            .VertEntry    = {.SourcePath = ShaderPath, .EntryPoint = "vertMain"},
            .FragEntry    = {.SourcePath = ShaderPath, .EntryPoint = "fragMain"},
            .Blend        = RHIBlendState{.Attachments = {RHIBlendAttachment{
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

    struct Parameter {
        /// Fused read-modify-write attachment: one internal RmwAttachment access, never a Read+Write pair.
        RGTextureView<RHITextureUsage::RenderTarget, RHIAccess::Read | RHIAccess::Write> SceneColor;
        RGTextureView<RHITextureUsage::Sampled, RHIAccess::Read>                         Mask;
        RHIRef<RHISampler>                                                               LinearSampler  = nullptr;
        Float32                                                                          ViewportWidth  = 0.0f;
        Float32                                                                          ViewportHeight = 0.0f;
    };

    SelectionOutlinePass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {}

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        m_Attachments = {};

        if (!m_Parameter->SceneColor.Resolved || !m_Parameter->Mask.Resolved || !m_Parameter->LinearSampler)
            return std::unexpected(ErrorMessage("Selection outline pass resources are not ready"));

        m_Attachments = RHIGraphicsAttachments{
            .ColorAttachments =
                {
                    {.View       = m_Parameter->SceneColor.ResolvedView(),
                     .ClearValue = std::array<Float32, 4>{0.0f, 0.0f, 0.0f, 0.0f},
                     .Clear      = false},
                },
        };
        SetViewport(0.0f,
                    0.0f,
                    static_cast<Float32>(m_Parameter->SceneColor.Resolved->GetWidth()),
                    static_cast<Float32>(m_Parameter->SceneColor.Resolved->GetHeight()));
        SetScissorRect(
            0, 0, m_Parameter->SceneColor.Resolved->GetWidth(), m_Parameter->SceneColor.Resolved->GetHeight());

        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_outline.maskTexture", m_Parameter->Mask.ResolvedView()},
                RHIShaderBindingRequest{"g_outline.sampler", RHISamplerView{.Sampler = m_Parameter->LinearSampler}},
            });
            !R)
            return std::unexpected(R.error());
        if (auto R = PushConstants(0,
                                   SelectionOutlinePushConstants{
                                       .ViewportWidth  = m_Parameter->ViewportWidth,
                                       .ViewportHeight = m_Parameter->ViewportHeight,
                                   });
            !R)
            return std::unexpected(R.error());
        Draw();
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

} // namespace SoulEngine
