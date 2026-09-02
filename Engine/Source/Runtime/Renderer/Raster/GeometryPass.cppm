module;

#include <hlsl++.h>

export module Renderer:Raster.GeometryPass;

import Core;
import RHI;
import Scene;

export import std;

export namespace SoulEngine {

struct GeometryPassInput {
    RHIRef<RHIRenderTarget>                    Albedo = nullptr;
    RHIRef<RHIRenderTarget>                    Normal = nullptr;
    RHIRef<RHIRenderTarget>                    MaterialId = nullptr;
    RHIRef<RHIRenderTarget>                    EntityId = nullptr;
    RHIRef<RHIRenderTarget>                    Depth = nullptr;
    RHIRef<RHISampler>                         LinearSampler = nullptr;
    RHIRef<RHISampler>                         AnisotropicSampler = nullptr;
    RHIRefArray<RHISampledTexture>             Textures = {};
    RHIRef<RHITransientShaderStorageBuffer>    LightBuffer = nullptr;
    RHIRef<RHITransientConstantBuffer>         FrameBuffer = nullptr;
    RHIRef<RHITransientConstantBuffer>         ViewBuffer = nullptr;
    RHIRef<RHITransientShaderStorageBuffer>    InstanceBuffer = nullptr;
    RHIRef<RHITransientShaderStorageBuffer>    GeometryBuffer = nullptr;
    RHIRef<RHITransientShaderStorageBuffer>    MaterialBuffer = nullptr;
    RHIRef<RHITransientShaderStorageBuffer>    IndirectBuffer = nullptr;
    Uint32                                      DrawCount = 0;
};

class GeometryPass final : public IRHIGraphicsPass {
  public:
    explicit GeometryPass(RHIRef<RHIGraphicsPipeline> Pipeline)
        : IRHIGraphicsPass(std::move(Pipeline)) {}

    auto SetInput(GeometryPassInput Input) -> void {
        m_Input = std::move(Input);
    }

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        m_Attachments = {};
        auto Input = std::move(m_Input);

        if (!GetShaderBindingSet(m_Pipeline) || !Input.Albedo || !Input.Normal || !Input.MaterialId || !Input.EntityId ||
            !Input.Depth || !Input.LinearSampler || !Input.AnisotropicSampler || !Input.Textures ||
            !Input.LightBuffer || !Input.FrameBuffer || !Input.ViewBuffer || !Input.InstanceBuffer ||
            !Input.GeometryBuffer || !Input.MaterialBuffer || !Input.IndirectBuffer || Input.DrawCount == 0)
            return std::unexpected(ErrorMessage("Geometry pass resources are not ready"));

        m_Attachments = RHIGraphicsAttachments{
            .ColorAttachments =
                {
                    {.TextureRef = Input.Albedo,
                     .ClearValue = std::array<Float32, 4>{0.025f, 0.035f, 0.055f, 1.0f}},
                    {.TextureRef = Input.Normal, .ClearValue = std::array<Float32, 4>{0.0f, 0.0f, 1.0f, 1.0f}},
                    {.TextureRef = Input.MaterialId,
                     .ClearValue = std::array<Float32, 4>{0.0f, 0.0f, 0.0f, 1.0f}},
                    {.TextureRef = Input.EntityId,
                     .ClearValue = std::array<Uint32, 4>{GBuffer::BackgroundEntityId, 0, 0, 0}},
                },
            .DepthAttachment = RHIDepthAttachmentDesc{
                .TextureRef = Input.Depth,
                .ClearValue = {.Depth = 1.0f, .Stencil = 0},
            },
        };
        SetViewport(0.0f,
                    0.0f,
                    static_cast<Float32>(Input.Albedo->GetWidth()),
                    static_cast<Float32>(Input.Albedo->GetHeight()));
        SetScissorRect(0, 0, Input.Albedo->GetWidth(), Input.Albedo->GetHeight());

        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_rasterFrameView.lights", std::move(Input.LightBuffer), true},
                RHIShaderBindingRequest{"g_rasterFrameView.frame", std::move(Input.FrameBuffer), true},
                RHIShaderBindingRequest{"g_rasterFrameView.view", std::move(Input.ViewBuffer), true},
            }); !R)
            return std::unexpected(R.error());
        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_samplers.uSamplerLinear", std::move(Input.LinearSampler), true},
                RHIShaderBindingRequest{"g_samplers.uSamplerAniso", std::move(Input.AnisotropicSampler), true},
            }); !R)
            return std::unexpected(R.error());
        if (auto R = BindBindlessResource(std::move(Input.Textures)); !R)
            return std::unexpected(R.error());
        if (auto R = BindResources(std::array{
                RHIShaderBindingRequest{"g_rasterDraw.instances", std::move(Input.InstanceBuffer), true},
                RHIShaderBindingRequest{"g_rasterDraw.geometries", std::move(Input.GeometryBuffer), true},
                RHIShaderBindingRequest{"g_rasterDraw.materials", std::move(Input.MaterialBuffer), true},
            }); !R)
            return std::unexpected(R.error());
        DrawIndirect(std::move(Input.IndirectBuffer), 0, Input.DrawCount);
        return {};
    }

  private:
    GeometryPassInput m_Input = {};
};

} // namespace SoulEngine
