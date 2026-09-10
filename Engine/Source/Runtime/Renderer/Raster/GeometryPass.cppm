module;

#include <hlsl++.h>

export module Renderer:RasterPasses.GeometryPass;

import Core;
import RHI;
import RenderGraph;
import Scene;

export import std;

export namespace SoulEngine {

class GeometryPass final : public IRHIGraphicsPass {
  public:
    static constexpr StringView Name = "GeometryPass";

    /// Static pipeline descriptor for PipelineRegistry::Register<GeometryPass>().
    [[nodiscard]] static auto BuildPipelineRequest() -> GraphicsPipelineRequest {
        const auto ShaderPath = ConfigManager::Get().EngineShadersDirPath() / "Raster" / "RasterGeometry.slang";
        return GraphicsPipelineRequest{
            .VertEntry    = {.SourcePath = ShaderPath, .EntryPoint = "vertMain"},
            .FragEntry    = {.SourcePath = ShaderPath, .EntryPoint = "fragMain"},
            .ColorFormats = std::vector<RHIFormat>(GBuffer::ColorFormats.begin(), GBuffer::ColorFormats.end()),
            .DepthFormat  = GBuffer::DepthFormat,
        };
    }

    /// The Parameter IS the resource declaration: view-typed fields derive
    /// accesses at AddPass; the remaining fields are passthrough (bindless
    /// table, samplers, draw count).
    struct Parameter {
        RGColorRT   Albedo     = {};
        RGColorRT   Normal     = {};
        RGColorRT   MaterialId = {};
        RGColorRT   EntityId   = {};
        RGDepthRT   Depth      = {};
        RGStorageBufferSRV InstanceBuffer = {};
        RGIndirectBuffer IndirectBuffer = {};
        RGStorageBufferSRV GeometryBuffer = {};
        RGStorageBufferSRV MaterialBuffer = {};
        RGConstantBufferSRV FrameBuffer = {};
        RGConstantBufferSRV ViewBuffer  = {};
        RHIRef<RHISampler>             LinearSampler      = nullptr;
        RHIRef<RHISampler>             AnisotropicSampler = nullptr;
        RHIRefArray<RHISampledTexture> Textures           = {};
        Uint32 DrawCount = 0;
    };

    GeometryPass(Parameter In, RHIRef<RHIGraphicsPipeline> Pipeline)
        : IRHIGraphicsPass(String(Name), std::move(Pipeline)), m_Parameter(std::move(In)) {}

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        m_Attachments = {};

        if (!m_Parameter.Albedo.Ref || !m_Parameter.Normal.Ref || !m_Parameter.MaterialId.Ref ||
            !m_Parameter.EntityId.Ref || !m_Parameter.Depth.Ref)
            return std::unexpected(ErrorMessage("Geometry pass resources are not ready: gbuffer attachment"));
        if (!m_Parameter.LinearSampler || !m_Parameter.AnisotropicSampler)
            return std::unexpected(ErrorMessage("Geometry pass resources are not ready: sampler"));
        if (!m_Parameter.Textures)
            return std::unexpected(ErrorMessage("Geometry pass resources are not ready: textures"));
        if (!m_Parameter.FrameBuffer.Ref || !m_Parameter.ViewBuffer.Ref)
            return std::unexpected(ErrorMessage("Geometry pass resources are not ready: frame/view constant buffer"));
        if (!m_Parameter.InstanceBuffer.Ref || !m_Parameter.GeometryBuffer.Ref ||
            !m_Parameter.MaterialBuffer.Ref || !m_Parameter.IndirectBuffer.Ref)
            return std::unexpected(ErrorMessage("Geometry pass resources are not ready: storage buffer"));
        if (m_Parameter.DrawCount == 0)
            return std::unexpected(ErrorMessage("Geometry pass resources are not ready: draw count"));

        m_Attachments = RHIGraphicsAttachments{
            .ColorAttachments =
                {
                    {.TextureRef = m_Parameter.Albedo.Ref,
                     .ClearValue = std::array<Float32, 4>{0.025f, 0.035f, 0.055f, 1.0f}},
                    {.TextureRef = m_Parameter.Normal.Ref, .ClearValue = std::array<Float32, 4>{0.0f, 0.0f, 1.0f, 1.0f}},
                    {.TextureRef = m_Parameter.MaterialId.Ref,
                     .ClearValue = std::array<Float32, 4>{0.0f, 0.0f, 0.0f, 1.0f}},
                    {.TextureRef = m_Parameter.EntityId.Ref,
                     .ClearValue = std::array<Uint32, 4>{GBuffer::BackgroundEntityId, 0, 0, 0}},
                },
            .DepthAttachment = RHIDepthAttachmentDesc{
                .TextureRef = m_Parameter.Depth.Ref,
                .ClearValue = {.Depth = 1.0f, .Stencil = 0},
            },
        };
        SetViewport(0.0f,
                    0.0f,
                    static_cast<Float32>(m_Parameter.Albedo.Ref->GetWidth()),
                    static_cast<Float32>(m_Parameter.Albedo.Ref->GetHeight()));
        SetScissorRect(0, 0, m_Parameter.Albedo.Ref->GetWidth(), m_Parameter.Albedo.Ref->GetHeight());

        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_rasterFrameView.frame", m_Parameter.FrameBuffer.Ref, true},
                RHIShaderBindingRequest{"g_rasterFrameView.view", m_Parameter.ViewBuffer.Ref, true},
                RHIShaderBindingRequest{"g_rasterFrameView.linearSampler", m_Parameter.LinearSampler, true},
            }); !R)
            return std::unexpected(R.error());
        if (auto R = BindBindlessResource(m_Parameter.Textures); !R)
            return std::unexpected(R.error());
        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_rasterDraw.instances", m_Parameter.InstanceBuffer.Ref, true},
                RHIShaderBindingRequest{"g_rasterDraw.geometryTable.records", m_Parameter.GeometryBuffer.Ref, true},
                RHIShaderBindingRequest{"g_rasterDraw.materials", m_Parameter.MaterialBuffer.Ref, true},
            }); !R)
            return std::unexpected(R.error());
        DrawIndirect(m_Parameter.IndirectBuffer.Ref, 0, m_Parameter.DrawCount);
        return {};
    }

  private:
    Parameter m_Parameter = {};
};

} // namespace SoulEngine
