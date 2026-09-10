module;

#include <hlsl++.h>

export module Renderer:EditorPasses.SelectionMaskPass;

import Core;
import Material;
import RHI;
import RenderGraph;
import Scene;

export import std;

export namespace SoulEngine {

class SelectionMaskPass final : public IRHIGraphicsPass {
  public:
    static constexpr StringView Name = "SelectionMaskPass";

    /// Static pipeline descriptor for PipelineRegistry::Register<SelectionMaskPass>().
    [[nodiscard]] static auto BuildPipelineRequest() -> GraphicsPipelineRequest {
        const auto ShaderPath = ConfigManager::Get().EngineShadersDirPath() / "Editor" / "SelectionMask.slang";
        return GraphicsPipelineRequest{
            .VertEntry    = {.SourcePath = ShaderPath, .EntryPoint = "vertMain"},
            .FragEntry    = {.SourcePath = ShaderPath, .EntryPoint = "fragMain"},
            .ColorFormats = {RHIFormat::R8_UNORM},
        };
    }

    struct Parameter {
        RGColorRT       Mask              = {};
        RGStorageBufferSRV SelectedInstances = {};
        RGIndirectBuffer  SelectedIndirect  = {};
        RGStorageBufferSRV GeometryTable     = {};
        RGStorageBufferSRV MaterialTable     = {};
        RGConstantBufferSRV FrameBuffer = {};
        RGConstantBufferSRV ViewBuffer  = {};
        RHIRef<RHISampler>             LinearSampler = nullptr;
        RHIRefArray<RHISampledTexture> Textures      = {};
        Uint32 DrawCount = 0;
    };

    SelectionMaskPass(Parameter In, RHIRef<RHIGraphicsPipeline> Pipeline)
        : IRHIGraphicsPass(String(Name), std::move(Pipeline)), m_Parameter(std::move(In)) {}

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        m_Attachments = {};

        if (!m_Parameter.Mask.Ref || !m_Parameter.FrameBuffer.Ref ||
            !m_Parameter.ViewBuffer.Ref || !m_Parameter.LinearSampler || !m_Parameter.Textures ||
            !m_Parameter.SelectedInstances.Ref || !m_Parameter.GeometryTable.Ref ||
            !m_Parameter.MaterialTable.Ref || !m_Parameter.SelectedIndirect.Ref || m_Parameter.DrawCount == 0)
            return std::unexpected(ErrorMessage("Selection mask pass resources are not ready"));

        m_Attachments = RHIGraphicsAttachments{
            .ColorAttachments = {
                {.TextureRef = m_Parameter.Mask.Ref,
                 .ClearValue = std::array<Float32, 4>{0.0f, 0.0f, 0.0f, 0.0f}},
            },
        };
        SetViewport(0.0f,
                    0.0f,
                    static_cast<Float32>(m_Parameter.Mask.Ref->GetWidth()),
                    static_cast<Float32>(m_Parameter.Mask.Ref->GetHeight()));
        SetScissorRect(0, 0, m_Parameter.Mask.Ref->GetWidth(), m_Parameter.Mask.Ref->GetHeight());

        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_selectionMaskFrameView.frame", m_Parameter.FrameBuffer.Ref, true},
                RHIShaderBindingRequest{"g_selectionMaskFrameView.view", m_Parameter.ViewBuffer.Ref, true},
                RHIShaderBindingRequest{"g_selectionMaskFrameView.linearSampler", m_Parameter.LinearSampler, true},
            }); !R)
            return std::unexpected(R.error());
        if (auto R = BindBindlessResource(m_Parameter.Textures); !R)
            return std::unexpected(R.error());
        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_selectionMaskDraw.instances", m_Parameter.SelectedInstances.Ref, true},
                RHIShaderBindingRequest{"g_selectionMaskDraw.geometryTable.records", m_Parameter.GeometryTable.Ref, true},
                RHIShaderBindingRequest{"g_selectionMaskDraw.materials", m_Parameter.MaterialTable.Ref, true},
            }); !R)
            return std::unexpected(R.error());
        DrawIndirect(m_Parameter.SelectedIndirect.Ref, 0, m_Parameter.DrawCount);
        return {};
    }

  private:
    Parameter m_Parameter = {};
};

} // namespace SoulEngine
