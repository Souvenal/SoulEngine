module;

#include <hlsl++.h>

export module Renderer:EditorPasses.SelectionMaskPass;

import Core;
import Material;
import RHI;
import RenderGraph;
import Scene;

import std;
import ShaderCompiler;

namespace SoulEngine {

export class SelectionMaskPass final : public IRHIGraphicsPass {
  public:
    static constexpr StringView Name = "SelectionMaskPass";
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    /// Static pipeline descriptor for PipelineRegistry::Register<SelectionMaskPass>().
    [[nodiscard]] static auto BuildPipelineRequest() -> GraphicsPipelineRequest {
        const auto ShaderPath = EnginePaths::Get().EngineShadersDirPath() / "Editor" / "SelectionMask.slang";
        return GraphicsPipelineRequest{
            .VertEntry    = {.SourcePath = ShaderPath, .EntryPoint = "vertMain"},
            .FragEntry    = {.SourcePath = ShaderPath, .EntryPoint = "fragMain"},
            .ColorFormats = {RHIFormat::R8_UNORM},
        };
    }

    struct Parameter {
        RGTextureView<RHITextureUsage::RenderTarget, RHIAccess::Write> Mask;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>         SelectedInstances;
        RGBufferView<RHIBufferUsage::Indirect, RHIAccess::Read>        SelectedIndirect;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>         GeometryTable;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>         MaterialTable;
        RGBufferView<RHIBufferUsage::Constant, RHIAccess::Read>        FrameBuffer;
        RGBufferView<RHIBufferUsage::Constant, RHIAccess::Read>        ViewBuffer;
        RHIRef<RHISampler>                                             LinearSampler = nullptr;
        Uint32                                                         DrawCount     = 0;
    };

    SelectionMaskPass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {}

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        m_Attachments = {};

        if (!m_Parameter->Mask.Resolved || !m_Parameter->FrameBuffer.Resolved || !m_Parameter->ViewBuffer.Resolved ||
            !m_Parameter->LinearSampler || !m_Parameter->SelectedInstances.Resolved ||
            !m_Parameter->GeometryTable.Resolved || !m_Parameter->MaterialTable.Resolved ||
            !m_Parameter->SelectedIndirect.Resolved || m_Parameter->DrawCount == 0)
            return std::unexpected(ErrorMessage("Selection mask pass resources are not ready"));

        m_Attachments = RHIGraphicsAttachments{
            .ColorAttachments =
                {
                    {.View       = m_Parameter->Mask.ResolvedView(),
                     .ClearValue = std::array<Float32, 4>{0.0f, 0.0f, 0.0f, 0.0f}},
                },
        };
        SetViewport(0.0f,
                    0.0f,
                    static_cast<Float32>(m_Parameter->Mask.Resolved->GetWidth()),
                    static_cast<Float32>(m_Parameter->Mask.Resolved->GetHeight()));
        SetScissorRect(0, 0, m_Parameter->Mask.Resolved->GetWidth(), m_Parameter->Mask.Resolved->GetHeight());

        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_selectionMaskFrameView.frame", m_Parameter->FrameBuffer.ResolvedView()},
                RHIShaderBindingRequest{"g_selectionMaskFrameView.view", m_Parameter->ViewBuffer.ResolvedView()},
                RHIShaderBindingRequest{"g_selectionMaskFrameView.linearSampler",
                                        RHISamplerView{.Sampler = m_Parameter->LinearSampler}},
            });
            !R)
            return std::unexpected(R.error());
        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_selectionMaskDraw.instances", m_Parameter->SelectedInstances.ResolvedView()},
                RHIShaderBindingRequest{"g_selectionMaskDraw.geometryTable.records",
                                        m_Parameter->GeometryTable.ResolvedView()},
                RHIShaderBindingRequest{"g_selectionMaskDraw.materials", m_Parameter->MaterialTable.ResolvedView()},
            });
            !R)
            return std::unexpected(R.error());
        DrawIndirect(m_Parameter->SelectedIndirect.Resolved, 0, m_Parameter->DrawCount);
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

} // namespace SoulEngine
