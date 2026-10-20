module;

#include <hlsl++.h>

export module Renderer:Raster.GeometryPass;

import Core;
import RHI;
import RenderGraph;
import :Common.GBuffer;

import std;
import ShaderCompiler;

namespace SoulEngine {

export class GeometryPass final : public IRHIGraphicsPass {
  public:
    static constexpr StringView Name = "GeometryPass";
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    /// Static pipeline descriptor for PipelineRegistry::Register<GeometryPass>().
    [[nodiscard]] static auto BuildPipelineRequest() -> GraphicsPipelineRequest {
        const auto ShaderPath = EnginePaths::Get().EngineShadersDirPath() / "Raster" / "RasterGeometry.slang";
        return GraphicsPipelineRequest{
            .VertEntry    = {.SourcePath = ShaderPath, .EntryPoint = "vertMain"},
            .FragEntry    = {.SourcePath = ShaderPath, .EntryPoint = "fragMain"},
            .ColorFormats = {RHIFormat::B8G8R8A8_UNORM,
                             RHIFormat::R16G16B16A16_SFLOAT,
                             RHIFormat::R32_UINT,
                             RHIFormat::R32_UINT},
            .DepthFormat  = RHIFormat::D32_SFLOAT,
        };
    }

    /// @brief The pass's resource declaration.
    ///
    /// The Parameter IS the resource declaration: each view declares its role and access here; the wiring
    /// site assigns identities only. The remaining fields are passthrough (bindless table, samplers, draw
    /// count).
    struct Parameter {
        RGTextureView<RHITextureUsage::RenderTarget, RHIAccess::Write> Albedo;
        RGTextureView<RHITextureUsage::RenderTarget, RHIAccess::Write> Normal;
        RGTextureView<RHITextureUsage::RenderTarget, RHIAccess::Write> MaterialId;
        RGTextureView<RHITextureUsage::RenderTarget, RHIAccess::Write> EntityId;
        RGTextureView<RHITextureUsage::DepthStencil, RHIAccess::Write> Depth;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>         InstanceBuffer;
        RGBufferView<RHIBufferUsage::Indirect, RHIAccess::Read>        IndirectBuffer;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>         GeometryBuffer;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>         MaterialBuffer;
        RGBufferView<RHIBufferUsage::Constant, RHIAccess::Read>        FrameBuffer;
        RGBufferView<RHIBufferUsage::Constant, RHIAccess::Read>        ViewBuffer;
        RHIRef<RHISampler>                                             LinearSampler      = nullptr;
        RHIRef<RHISampler>                                             AnisotropicSampler = nullptr;
        Uint32                                                         DrawCount          = 0;
    };

    GeometryPass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {}

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        m_Attachments = {};

        if (!m_Parameter->Albedo.Resolved || !m_Parameter->Normal.Resolved || !m_Parameter->MaterialId.Resolved ||
            !m_Parameter->EntityId.Resolved || !m_Parameter->Depth.Resolved)
            return std::unexpected(ErrorMessage("Geometry pass resources are not ready: gbuffer attachment"));
        if (!m_Parameter->LinearSampler || !m_Parameter->AnisotropicSampler)
            return std::unexpected(ErrorMessage("Geometry pass resources are not ready: sampler"));
        if (!m_Parameter->FrameBuffer.Resolved || !m_Parameter->ViewBuffer.Resolved)
            return std::unexpected(ErrorMessage("Geometry pass resources are not ready: frame/view constant buffer"));
        if (!m_Parameter->InstanceBuffer.Resolved || !m_Parameter->GeometryBuffer.Resolved ||
            !m_Parameter->MaterialBuffer.Resolved || !m_Parameter->IndirectBuffer.Resolved)
            return std::unexpected(ErrorMessage("Geometry pass resources are not ready: storage buffer"));
        if (m_Parameter->DrawCount == 0)
            return std::unexpected(ErrorMessage("Geometry pass resources are not ready: draw count"));

        m_Attachments = RHIGraphicsAttachments{
            .ColorAttachments =
                {
                    {.View       = m_Parameter->Albedo.ResolvedView(),
                     .ClearValue = std::array<Float32, 4>{0.025f, 0.035f, 0.055f, 1.0f}},
                    {.View       = m_Parameter->Normal.ResolvedView(),
                     .ClearValue = std::array<Float32, 4>{0.0f, 0.0f, 1.0f, 1.0f}},
                    {.View       = m_Parameter->MaterialId.ResolvedView(),
                     .ClearValue = std::array<Float32, 4>{0.0f, 0.0f, 0.0f, 1.0f}},
                    {.View       = m_Parameter->EntityId.ResolvedView(),
                     .ClearValue = std::array<Uint32, 4>{GBuffer::BackgroundEntityId, 0, 0, 0}},
                },
            .DepthAttachment =
                RHIDepthAttachmentDesc{
                    .View       = m_Parameter->Depth.ResolvedView(),
                    .ClearValue = {.Depth = 1.0f, .Stencil = 0},
                },
        };
        SetViewport(0.0f,
                    0.0f,
                    static_cast<Float32>(m_Parameter->Albedo.Resolved->GetWidth()),
                    static_cast<Float32>(m_Parameter->Albedo.Resolved->GetHeight()));
        SetScissorRect(0, 0, m_Parameter->Albedo.Resolved->GetWidth(), m_Parameter->Albedo.Resolved->GetHeight());

        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_rasterFrameView.frame", m_Parameter->FrameBuffer.ResolvedView()},
                RHIShaderBindingRequest{"g_rasterFrameView.view", m_Parameter->ViewBuffer.ResolvedView()},
                RHIShaderBindingRequest{"g_rasterFrameView.linearSampler",
                                        RHISamplerView{.Sampler = m_Parameter->LinearSampler}},
            });
            !R)
            return std::unexpected(R.error());
        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_rasterDraw.instances", m_Parameter->InstanceBuffer.ResolvedView()},
                RHIShaderBindingRequest{"g_rasterDraw.geometryTable.records",
                                        m_Parameter->GeometryBuffer.ResolvedView()},
                RHIShaderBindingRequest{"g_rasterDraw.materials", m_Parameter->MaterialBuffer.ResolvedView()},
            });
            !R)
            return std::unexpected(R.error());
        DrawIndirect(m_Parameter->IndirectBuffer.Resolved, 0, m_Parameter->DrawCount);
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

} // namespace SoulEngine
