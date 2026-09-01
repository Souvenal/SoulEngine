module;

#include <hlsl++.h>

export module Renderer:Raster.DeferredLightingPass;

import Core;
import RHI;

export import std;

export namespace SoulEngine {

struct DeferredLightingPassInput {
    RHIRef<RHIRenderTarget>                  SceneColor = nullptr;
    RHIRef<RHIRenderTarget>                  Albedo = nullptr;
    RHIRef<RHIRenderTarget>                  Normal = nullptr;
    RHIRef<RHIRenderTarget>                  MaterialId = nullptr;
    RHIRef<RHIRenderTarget>                  EntityId = nullptr;
    RHIRef<RHIRenderTarget>                  Depth = nullptr;
    RHIRef<RHISampler>                       LinearSampler = nullptr;
    RHIRef<RHISampler>                       AnisotropicSampler = nullptr;
    RHIRefArray<RHISampledTexture>           Textures = {};
    RHIRef<RHITransientConstantBuffer>       FrameBuffer = nullptr;
    RHIRef<RHITransientConstantBuffer>       ViewBuffer = nullptr;
    RHIRef<RHITransientShaderStorageBuffer> MaterialBuffer = nullptr;
    RHIRef<RHITransientShaderStorageBuffer> LightBuffer = nullptr;
};

class DeferredLightingPass final : public IRHIGraphicsPass {
  public:
    explicit DeferredLightingPass(RHIRef<RHIGraphicsPipeline> Pipeline)
        : IRHIGraphicsPass(std::move(Pipeline)) {}

    auto SetInput(DeferredLightingPassInput Input) -> void {
        m_Input = std::move(Input);
    }

    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        m_Attachments = {};
        auto Input = std::move(m_Input);

        if (!GetShaderBindingSet(m_Pipeline) || !Input.SceneColor || !Input.Albedo || !Input.Normal || !Input.MaterialId ||
            !Input.EntityId || !Input.Depth || !Input.LinearSampler || !Input.AnisotropicSampler || !Input.Textures ||
            !Input.FrameBuffer || !Input.ViewBuffer || !Input.MaterialBuffer || !Input.LightBuffer)
            return std::unexpected(ErrorMessage("Deferred lighting pass resources are not ready"));

        m_Attachments = RHIGraphicsAttachments{
            .ColorAttachments = {
                {.TextureRef = Input.SceneColor,
                 .ClearValue = std::array<Float32, 4>{0.025f, 0.035f, 0.055f, 1.0f}},
            },
        };
        SetViewport(0.0f,
                    0.0f,
                    static_cast<Float32>(Input.SceneColor->GetWidth()),
                    static_cast<Float32>(Input.SceneColor->GetHeight()));
        SetScissorRect(0, 0, Input.SceneColor->GetWidth(), Input.SceneColor->GetHeight());

        if (auto R = BindResource("g_gbuffer.albedo", std::move(Input.Albedo), true); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_gbuffer.normal", std::move(Input.Normal), true); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_gbuffer.materialId", std::move(Input.MaterialId), true); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_gbuffer.entityId", std::move(Input.EntityId), true); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_gbuffer.depth", std::move(Input.Depth), true); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_samplers.uSamplerLinear", std::move(Input.LinearSampler), true); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_samplers.uSamplerAniso", std::move(Input.AnisotropicSampler), true); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource(std::move(Input.Textures)); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_deferred.frame", std::move(Input.FrameBuffer), true); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_deferred.view", std::move(Input.ViewBuffer), true); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_deferred.materials", std::move(Input.MaterialBuffer), true); !R)
            return std::unexpected(R.error());
        if (auto R = BindResource("g_deferred.lights", std::move(Input.LightBuffer), true); !R)
            return std::unexpected(R.error());
        if (auto R = Commit(); !R)
            return std::unexpected(R.error());
        Draw();
        return {};
    }

  private:
    DeferredLightingPassInput m_Input = {};
};

} // namespace SoulEngine
