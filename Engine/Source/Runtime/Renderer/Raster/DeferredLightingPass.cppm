module;

#include <hlsl++.h>

export module Renderer:Raster.DeferredLightingPass;

import Core;
import RHI;
import RenderGraph;

import std;
import ShaderCompiler;

namespace SoulEngine {

/// @brief Deferred lighting over the GBuffer.
///
/// The class IS the graph's TPass. SceneColor is a plain color write; SelectionOutline may overblend it (load
/// RMW) and the explicit BlitToSwapchainPass presents it.
export class DeferredLightingPass final : public IRHIGraphicsPass {
  public:
    static constexpr StringView Name = "DeferredLightingPass";
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    /// Static pipeline descriptor for PipelineRegistry::Register<DeferredLightingPass>().
    [[nodiscard]] static auto BuildPipelineRequest() -> GraphicsPipelineRequest {
        const auto ShaderPath = EnginePaths::Get().EngineShadersDirPath() / "Raster" / "DeferredLighting.slang";
        return GraphicsPipelineRequest{
            .VertEntry    = {.SourcePath = ShaderPath, .EntryPoint = "vertMain"},
            .FragEntry    = {.SourcePath = ShaderPath, .EntryPoint = "fragMain"},
            .ColorFormats = {RHIFormat::B8G8R8A8_UNORM},
        };
    }

    struct Parameter {
        RGTextureView<RHITextureUsage::RenderTarget, RHIAccess::Write> SceneColor;
        RGTextureView<RHITextureUsage::Sampled, RHIAccess::Read>       Albedo;
        RGTextureView<RHITextureUsage::Sampled, RHIAccess::Read>       Normal;
        RGTextureView<RHITextureUsage::Sampled, RHIAccess::Read>       MaterialId;
        RGTextureView<RHITextureUsage::Sampled, RHIAccess::Read>       EntityId;
        RGTextureView<RHITextureUsage::Sampled, RHIAccess::Read>       Depth;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>         MaterialBuffer;
        RGBufferView<RHIBufferUsage::Constant, RHIAccess::Read>        FrameBuffer;
        RGBufferView<RHIBufferUsage::Constant, RHIAccess::Read>        ViewBuffer;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>         LightBuffer;
        RHIRef<RHISampler>                                             LinearSampler      = nullptr;
        RHIRef<RHISampler>                                             AnisotropicSampler = nullptr;
    };

    DeferredLightingPass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {}

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        m_Attachments = {};

        if (!m_Parameter->SceneColor.Resolved || !m_Parameter->Albedo.Resolved || !m_Parameter->Normal.Resolved ||
            !m_Parameter->MaterialId.Resolved || !m_Parameter->EntityId.Resolved || !m_Parameter->Depth.Resolved ||
            !m_Parameter->FrameBuffer.Resolved || !m_Parameter->ViewBuffer.Resolved ||
            !m_Parameter->MaterialBuffer.Resolved || !m_Parameter->LightBuffer.Resolved)
            return std::unexpected(ErrorMessage("Deferred lighting pass resources are not ready"));

        m_Attachments = RHIGraphicsAttachments{
            .ColorAttachments =
                {
                    {.View       = m_Parameter->SceneColor.ResolvedView(),
                     .ClearValue = std::array<Float32, 4>{0.025f, 0.035f, 0.055f, 1.0f}},
                },
        };
        SetViewport(0.0f,
                    0.0f,
                    static_cast<Float32>(m_Parameter->SceneColor.Resolved->GetWidth()),
                    static_cast<Float32>(m_Parameter->SceneColor.Resolved->GetHeight()));
        SetScissorRect(
            0, 0, m_Parameter->SceneColor.Resolved->GetWidth(), m_Parameter->SceneColor.Resolved->GetHeight());

        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_gbuffer.albedo", m_Parameter->Albedo.ResolvedView()},
                RHIShaderBindingRequest{"g_gbuffer.normal", m_Parameter->Normal.ResolvedView()},
                RHIShaderBindingRequest{"g_gbuffer.materialId", m_Parameter->MaterialId.ResolvedView()},
                RHIShaderBindingRequest{"g_gbuffer.entityId", m_Parameter->EntityId.ResolvedView()},
                RHIShaderBindingRequest{"g_gbuffer.depth", m_Parameter->Depth.ResolvedView()},
            });
            !R)
            return std::unexpected(R.error());
        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_deferred.frame", m_Parameter->FrameBuffer.ResolvedView()},
                RHIShaderBindingRequest{"g_deferred.view", m_Parameter->ViewBuffer.ResolvedView()},
                RHIShaderBindingRequest{"g_deferred.materials", m_Parameter->MaterialBuffer.ResolvedView()},
                RHIShaderBindingRequest{"g_deferred.lights", m_Parameter->LightBuffer.ResolvedView()},
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
