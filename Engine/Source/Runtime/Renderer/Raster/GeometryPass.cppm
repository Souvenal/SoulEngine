module;

#include <hlsl++.h>

export module Renderer.Raster.GeometryPass;

import Core;
import RHI;
import RenderGraph;
import Renderer.Common.GBuffer;

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
            .VertEntry        = {.SourcePath = ShaderPath, .EntryPoint = "vertMain"},
            .FragEntry        = {.SourcePath = ShaderPath, .EntryPoint = "fragMain"},
            .ColorAttachments = {{.Format = RHIFormat::B8G8R8A8_UNORM},
                                 {.Format = RHIFormat::R16G16B16A16_SFLOAT},
                                 {.Format = RHIFormat::R32_UINT},
                                 {.Format = RHIFormat::R32_UINT}},
            .DepthFormat      = RHIFormat::D32_SFLOAT,
        };
    }

    /// @brief The pass's resource declaration.
    ///
    /// The Parameter IS the resource declaration: each view declares its role and access here; the wiring
    /// site assigns identities only. The remaining fields are passthrough (bindless table, samplers, draw
    /// count).
    struct Parameter {
        RGTextureView<RHITextureUsage::RenderTarget, RHIAccess::Write>      Albedo;
        RGTextureView<RHITextureUsage::RenderTarget, RHIAccess::Write>      Normal;
        RGTextureView<RHITextureUsage::RenderTarget, RHIAccess::Write>      MaterialId;
        RGTextureView<RHITextureUsage::RenderTarget, RHIAccess::Write>      EntityId;
        RGTextureView<RHITextureUsage::DepthStencil, RHIAccess::Write>      Depth;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>              InstanceBuffer;
        RGBufferView<RHIBufferUsage::Indirect, RHIAccess::Read>             IndirectBuffer;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>              GeometryBuffer;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>              MaterialBuffer;
        RGBufferView<RHIBufferUsage::Constant, RHIAccess::Read>             FrameBuffer;
        RGBufferView<RHIBufferUsage::Constant, RHIAccess::Read>             ViewBuffer;
        std::vector<RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>> AttributeBuffers   = {};
        std::vector<RGBufferView<RHIBufferUsage::Index, RHIAccess::Read>>   IndexBuffers       = {};
        RHIRef<RHISampler>                                                  LinearSampler      = nullptr;
        RHIRef<RHISampler>                                                  AnisotropicSampler = nullptr;
        Uint32                                                              DrawCount          = 0;
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
        if (m_Parameter->IndexBuffers.size() != m_Parameter->DrawCount)
            return std::unexpected(ErrorMessage("Geometry pass resources are not ready: index buffer count"));
        for (const auto& View : m_Parameter->AttributeBuffers) {
            if (!View.Resolved)
                return std::unexpected(ErrorMessage("Geometry pass resources are not ready: attribute buffer"));
        }
        for (const auto& View : m_Parameter->IndexBuffers) {
            if (!View.Resolved)
                return std::unexpected(ErrorMessage("Geometry pass resources are not ready: index buffer"));
        }

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
                RHIShaderBindingRequest{"g_rasterDraw.materials", m_Parameter->MaterialBuffer.ResolvedView()},
            });
            !R)
            return std::unexpected(R.error());
        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_rasterPulling.geometryTable.records",
                                        m_Parameter->GeometryBuffer.ResolvedView()},
            });
            !R)
            return std::unexpected(R.error());
        for (Uint32 Slot = 0; Slot < m_Parameter->DrawCount; ++Slot) {
            BindIndexBuffer(m_Parameter->IndexBuffers[Slot].Resolved);
            DrawIndexedIndirect(m_Parameter->IndirectBuffer.Resolved, Slot * sizeof(Uint32) * 5);
        }
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

export class VertexInputGeometryPass final : public IRHIGraphicsPass {
  public:
    static constexpr StringView Name = "VertexInputGeometryPass";
    [[nodiscard]] auto          GetName() const noexcept -> StringView override {
        return Name;
    }

    /// Static pipeline descriptor for PipelineRegistry::Register<VertexInputGeometryPass>().
    [[nodiscard]] static auto BuildPipelineRequest() -> GraphicsPipelineRequest {
        const auto ShaderPath = EnginePaths::Get().EngineShadersDirPath() / "Raster" / "RasterGeometry.slang";
        return GraphicsPipelineRequest{
            .VertEntry = {.SourcePath = ShaderPath, .EntryPoint = "vertVertexInputMain"},
            .FragEntry = {.SourcePath = ShaderPath, .EntryPoint = "fragMain"},
            .VertexInputLayout =
                RHIVertexInputLayoutDesc{
                    .Bindings   = {{.Binding = 0, .Stride = sizeof(Float32) * 3},
                                   {.Binding = 1, .Stride = sizeof(Float32) * 3},
                                   {.Binding = 2, .Stride = sizeof(Float32) * 4},
                                   {.Binding = 3, .Stride = sizeof(Float32) * 2}},
                    .Attributes = {{.Location = 0, .Binding = 0, .Format = RHIFormat::R32G32B32_SFLOAT, .Offset = 0},
                                   {.Location = 1, .Binding = 1, .Format = RHIFormat::R32G32B32_SFLOAT, .Offset = 0},
                                   {.Location = 2, .Binding = 2, .Format = RHIFormat::R32G32B32A32_SFLOAT, .Offset = 0},
                                   {.Location = 3, .Binding = 3, .Format = RHIFormat::R32G32_SFLOAT, .Offset = 0}}},
            .ColorAttachments = {{.Format = RHIFormat::B8G8R8A8_UNORM},
                                 {.Format = RHIFormat::R16G16B16A16_SFLOAT},
                                 {.Format = RHIFormat::R32_UINT},
                                 {.Format = RHIFormat::R32_UINT}},
            .DepthFormat      = RHIFormat::D32_SFLOAT,
        };
    }

    /// @brief The pass's resource declaration.
    struct Parameter {
        RGTextureView<RHITextureUsage::RenderTarget, RHIAccess::Write>     Albedo;
        RGTextureView<RHITextureUsage::RenderTarget, RHIAccess::Write>     Normal;
        RGTextureView<RHITextureUsage::RenderTarget, RHIAccess::Write>     MaterialId;
        RGTextureView<RHITextureUsage::RenderTarget, RHIAccess::Write>     EntityId;
        RGTextureView<RHITextureUsage::DepthStencil, RHIAccess::Write>     Depth;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>             InstanceBuffer;
        RGBufferView<RHIBufferUsage::Indirect, RHIAccess::Read>            IndirectBuffer;
        RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>             MaterialBuffer;
        RGBufferView<RHIBufferUsage::Constant, RHIAccess::Read>            FrameBuffer;
        RGBufferView<RHIBufferUsage::Constant, RHIAccess::Read>            ViewBuffer;
        std::vector<RGBufferView<RHIBufferUsage::Vertex, RHIAccess::Read>> VertexBuffers      = {};
        std::vector<RGBufferView<RHIBufferUsage::Index, RHIAccess::Read>>  IndexBuffers       = {};
        RHIRef<RHISampler>                                                 LinearSampler      = nullptr;
        RHIRef<RHISampler>                                                 AnisotropicSampler = nullptr;
        Uint32                                                             DrawCount          = 0;
    };

    VertexInputGeometryPass(UPtr<Parameter> In) : m_Parameter(std::move(In)) {}

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        m_Attachments = {};

        if (!m_Parameter->Albedo.Resolved || !m_Parameter->Normal.Resolved || !m_Parameter->MaterialId.Resolved ||
            !m_Parameter->EntityId.Resolved || !m_Parameter->Depth.Resolved)
            return std::unexpected(
                ErrorMessage("VertexInput geometry pass resources are not ready: gbuffer attachment"));
        if (!m_Parameter->LinearSampler || !m_Parameter->AnisotropicSampler)
            return std::unexpected(ErrorMessage("VertexInput geometry pass resources are not ready: sampler"));
        if (!m_Parameter->FrameBuffer.Resolved || !m_Parameter->ViewBuffer.Resolved)
            return std::unexpected(
                ErrorMessage("VertexInput geometry pass resources are not ready: frame/view constant buffer"));
        if (!m_Parameter->InstanceBuffer.Resolved || !m_Parameter->MaterialBuffer.Resolved ||
            !m_Parameter->IndirectBuffer.Resolved)
            return std::unexpected(ErrorMessage("VertexInput geometry pass resources are not ready: storage buffer"));
        if (m_Parameter->DrawCount == 0)
            return std::unexpected(ErrorMessage("VertexInput geometry pass resources are not ready: draw count"));
        if (m_Parameter->IndexBuffers.size() != m_Parameter->DrawCount)
            return std::unexpected(
                ErrorMessage("VertexInput geometry pass resources are not ready: index buffer count"));
        if (m_Parameter->VertexBuffers.size() != m_Parameter->DrawCount * kMaxVertexBufferBindings)
            return std::unexpected(
                ErrorMessage("VertexInput geometry pass resources are not ready: vertex buffer count"));
        for (const auto& View : m_Parameter->VertexBuffers) {
            if (!View.Resolved)
                return std::unexpected(
                    ErrorMessage("VertexInput geometry pass resources are not ready: vertex buffer"));
        }
        for (const auto& View : m_Parameter->IndexBuffers) {
            if (!View.Resolved)
                return std::unexpected(ErrorMessage("VertexInput geometry pass resources are not ready: index buffer"));
        }

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
                RHIShaderBindingRequest{"g_rasterDraw.materials", m_Parameter->MaterialBuffer.ResolvedView()},
            });
            !R)
            return std::unexpected(R.error());
        for (Uint32 Slot = 0; Slot < m_Parameter->DrawCount; ++Slot) {
            const auto First = Slot * kMaxVertexBufferBindings;
            BindVertexBuffers(std::array{
                RHIVertexBufferBinding{.Buffer = m_Parameter->VertexBuffers[First].Resolved},
                RHIVertexBufferBinding{.Buffer = m_Parameter->VertexBuffers[First + 1].Resolved},
                RHIVertexBufferBinding{.Buffer = m_Parameter->VertexBuffers[First + 2].Resolved},
                RHIVertexBufferBinding{.Buffer = m_Parameter->VertexBuffers[First + 3].Resolved},
            });
            BindIndexBuffer(m_Parameter->IndexBuffers[Slot].Resolved);
            DrawIndexedIndirect(m_Parameter->IndirectBuffer.Resolved, Slot * sizeof(Uint32) * 5);
        }
        return {};
    }

  private:
    UPtr<Parameter> m_Parameter;
};

} // namespace SoulEngine
