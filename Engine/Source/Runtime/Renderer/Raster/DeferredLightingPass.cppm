module;

#include <hlsl++.h>

export module Renderer:RasterPasses.DeferredLightingPass;

import Core;
import RHI;
import RenderGraph;

export import std;

export namespace SoulEngine {

/// @brief Deferred lighting over the GBuffer.
///
/// The class IS the graph's TPass. The frame config picks SceneColor's write
/// kind at AddPass time: `RGColorRT{.Present = true}` is the terminal present
/// write; a plain `RGColorRT` feeds SelectionOutline's load RMW, which carries
/// the Present.
class DeferredLightingPass final : public IRHIGraphicsPass {
  public:
    static constexpr StringView Name = "DeferredLightingPass";

    /// Static pipeline descriptor for PipelineRegistry::Register<DeferredLightingPass>().
    [[nodiscard]] static auto BuildPipelineRequest() -> GraphicsPipelineRequest {
        const auto ShaderPath = ConfigManager::Get().EngineShadersDirPath() / "Raster" / "DeferredLighting.slang";
        return GraphicsPipelineRequest{
            .VertEntry    = {.SourcePath = ShaderPath, .EntryPoint = "vertMain"},
            .FragEntry    = {.SourcePath = ShaderPath, .EntryPoint = "fragMain"},
            .ColorFormats = {RHIFormat::B8G8R8A8_UNORM},
        };
    }

    struct Parameter {
        RGColorRT    SceneColor = {};
        RGTextureSRV Albedo     = {};
        RGTextureSRV Normal     = {};
        RGTextureSRV MaterialId = {};
        RGTextureSRV EntityId   = {};
        RGTextureSRV Depth      = {};
        RGStorageBufferSRV     MaterialBuffer = {};
        RGConstantBufferSRV FrameBuffer    = {};
        RGConstantBufferSRV ViewBuffer     = {};
        RGStorageBufferSRV     LightBuffer    = {};
        RHIRef<RHISampler>             LinearSampler      = nullptr;
        RHIRef<RHISampler>             AnisotropicSampler = nullptr;
        RHIRefArray<RHISampledTexture> Textures           = {};
    };

    DeferredLightingPass(Parameter In, RHIRef<RHIGraphicsPipeline> Pipeline)
        : IRHIGraphicsPass(String(Name), std::move(Pipeline)), m_Parameter(std::move(In)) {}

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        m_Attachments = {};

        if (!m_Parameter.SceneColor.Ref || !m_Parameter.Albedo.Ref ||
            !m_Parameter.Normal.Ref || !m_Parameter.MaterialId.Ref || !m_Parameter.EntityId.Ref ||
            !m_Parameter.Depth.Ref || !m_Parameter.Textures || !m_Parameter.FrameBuffer.Ref ||
            !m_Parameter.ViewBuffer.Ref || !m_Parameter.MaterialBuffer.Ref || !m_Parameter.LightBuffer.Ref)
            return std::unexpected(ErrorMessage("Deferred lighting pass resources are not ready"));

        m_Attachments = RHIGraphicsAttachments{
            .ColorAttachments = {
                {.TextureRef = m_Parameter.SceneColor.Ref,
                 .ClearValue = std::array<Float32, 4>{0.025f, 0.035f, 0.055f, 1.0f}},
            },
        };
        SetViewport(0.0f,
                    0.0f,
                    static_cast<Float32>(m_Parameter.SceneColor.Ref->GetWidth()),
                    static_cast<Float32>(m_Parameter.SceneColor.Ref->GetHeight()));
        SetScissorRect(0, 0, m_Parameter.SceneColor.Ref->GetWidth(), m_Parameter.SceneColor.Ref->GetHeight());

        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_gbuffer.albedo", m_Parameter.Albedo.Ref, true},
                RHIShaderBindingRequest{"g_gbuffer.normal", m_Parameter.Normal.Ref, true},
                RHIShaderBindingRequest{"g_gbuffer.materialId", m_Parameter.MaterialId.Ref, true},
                RHIShaderBindingRequest{"g_gbuffer.entityId", m_Parameter.EntityId.Ref, true},
                RHIShaderBindingRequest{"g_gbuffer.depth", m_Parameter.Depth.Ref, true},
            }); !R)
            return std::unexpected(R.error());
        if (auto R = BindBindlessResource(m_Parameter.Textures); !R)
            return std::unexpected(R.error());
        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_deferred.frame", m_Parameter.FrameBuffer.Ref, true},
                RHIShaderBindingRequest{"g_deferred.view", m_Parameter.ViewBuffer.Ref, true},
                RHIShaderBindingRequest{"g_deferred.materials", m_Parameter.MaterialBuffer.Ref, true},
                RHIShaderBindingRequest{"g_deferred.lights", m_Parameter.LightBuffer.Ref, true},
            }); !R)
            return std::unexpected(R.error());
        Draw();
        return {};
    }

  private:
    Parameter m_Parameter = {};
};

} // namespace SoulEngine
