module;

// stddef is explicitly needed for `offsetof`
#include <cstddef>
#include <hlsl++.h>

export module Renderer:TestRenderer;

import Core;
import RHI;
import Scene;
import Resource;

import :IRenderer;

export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Renderer {

/// @brief Constant buffer layout matching Common.slang FrameData.
struct alignas(16) FrameConstants {
    alignas(16) float Time = 0.0f;
};
static_assert(sizeof(FrameConstants) == 16, "FrameConstants must match Common.slang FrameData std140 layout");

struct ViewParameterState {
    String                ViewConstantBufferKey = {};
    RHI::ShaderParameters Parameters            = {};
};

/// @brief Minimal prototype renderer that emits a CommandList for RHIThread.
class TestRenderer final : public IRenderer {
  public:
    TestRenderer() = default;
    ~TestRenderer() override {
        OnDetach();
    }

    [[nodiscard]] auto OnAttach() -> std::expected<void, ErrorMessage> override {
        auto& Ctx = RHI::RenderDevice::Get();

        auto ShaderDir = ConfigManager::Get().CurrentApplicationDir() / "Shaders";
        m_Pipeline     = Resource::Manager::Get()
                             .RequestGraphicsPipelineRef(
                                 Resource::GraphicsPipelineRequest{
                                     .VertEntry =
                                         {
                                             .SourcePath = Path((ShaderDir / "CubeGlobalCB.slang").string()),
                                             .EntryPoint = "vertMain",
                                         },
                                     .FragEntry =
                                         {
                                             .SourcePath = Path((ShaderDir / "CubeGlobalCB.slang").string()),
                                             .EntryPoint = "fragMain",
                                         },
                                     .VertexInputLayout =
                                         RHI::VertexInputLayoutDesc{
                                             .Bindings =
                                                 {
                                                     {.Binding = 0, .Stride = sizeof(hlslpp::interop::float3)},
                                                     {.Binding = 1, .Stride = sizeof(hlslpp::interop::float3)},
                                                     {.Binding = 2, .Stride = sizeof(hlslpp::interop::float4)},
                                                     {.Binding = 3, .Stride = sizeof(hlslpp::interop::float2)},
                                                 },
                                             .Attributes =
                                                 {
                                                     RHI::VertexInputAttributeDesc{
                                                         .Location = 0,
                                                         .Binding  = 0,
                                                         .Format   = RHI::Format::R32G32B32_SFLOAT,
                                                         .Offset   = 0,
                                                     },
                                                     RHI::VertexInputAttributeDesc{
                                                         .Location = 1,
                                                         .Binding  = 1,
                                                         .Format   = RHI::Format::R32G32B32_SFLOAT,
                                                         .Offset   = 0,
                                                     },
                                                     RHI::VertexInputAttributeDesc{
                                                         .Location = 2,
                                                         .Binding  = 2,
                                                         .Format   = RHI::Format::R32G32B32A32_SFLOAT,
                                                         .Offset   = 0,
                                                     },
                                                     RHI::VertexInputAttributeDesc{
                                                         .Location = 3,
                                                         .Binding  = 3,
                                                         .Format   = RHI::Format::R32G32_SFLOAT,
                                                         .Offset   = 0,
                                                     },
                                                 },
                                         },
                                     .DepthFormat = RHI::Format::D32_SFLOAT,
                                 });
        if (!m_Pipeline)
            return std::unexpected(ErrorMessage("Graphics pipeline request failed"));

        m_FrameConstants =
            Resource::Manager::Get().RequestConstantBufferRef("frame_constants", {.Size = sizeof(FrameConstants)});
        if (!m_FrameConstants)
            return std::unexpected(ErrorMessage("Frame constant buffer request failed"));

        m_LinearSampler = Resource::Manager::Get().RequestSamplerRef(RHI::SamplerDesc{});
        if (!m_LinearSampler)
            return std::unexpected(ErrorMessage("Linear sampler request failed"));

        m_AnisoSampler = Resource::Manager::Get().RequestSamplerRef(
            RHI::SamplerDesc{.Profile = RHI::SamplerProfile::AnisotropicRepeat});
        if (!m_AnisoSampler)
            return std::unexpected(ErrorMessage("Anisotropic sampler request failed"));

        auto Texture = Resource::Manager::Get().RequestSampledTextureRef(
            (ConfigManager::Get().CurrentApplicationDir() / "Assets" / "statue.jpg").string());
        if (!Texture)
            return std::unexpected(ErrorMessage("Test texture request failed"));

        Resource::Array<RHI::SampledTexture> Textures;
        if (auto R = Textures.Set(0, std::move(Texture)); !R)
            return std::unexpected(R.error().Append("Test texture array initialization failed"));
        m_Textures = std::move(Textures);

        return {};
    }

    auto OnDetach() -> void override {
        m_Pipeline       = {};
        m_FrameConstants = {};
        m_LinearSampler  = {};
        m_AnisoSampler   = {};
        m_Textures.reset();
        m_ViewParameters.clear();
    }

    /// Produce a CommandList from the current scene snapshot.
    /// Called by RenderLoop. Does NOT call BeginFrame/EndFrame.
    [[nodiscard]] auto Render(const Scene::SceneSnapshot& Scene) -> std::expected<RenderResult, ErrorMessage> override {
        RenderResult Result;
        if (Scene.Views.empty())
            return Result;

        auto* FrameCB = Resource::Manager::Get().TryGetReady(m_FrameConstants);
        if (!FrameCB)
            return Result;

        const auto FrameData = BuildFrameConstants(Scene.Time);
        for (const auto& View : Scene.Views) {
            if (auto R = RenderView(Result.CmdList, Scene, View, FrameCB, FrameData); !R)
                return std::unexpected(R.error());
        }

        return Result;
    }

  private:
    [[nodiscard]] auto RenderView(RHI::CommandList&                    CmdList,
                                  const Scene::SceneSnapshot&        Scene,
                                  const Scene::RenderViewSnapshot&    View,
                                  RHI::ConstantBuffer*                FrameCB,
                                  const FrameConstants&               FrameData)
        -> std::expected<void, ErrorMessage> {
        auto* ColorRT = Resource::Manager::Get().TryGetReady(View.ColorRT);
        if (!ColorRT)
            return {};

        // Build pass — backend wraps each Pass in begin/end rendering.
        RHI::Pass Pass;
        Pass.Desc = RHI::RenderingDesc{
            .ColorAttachment =
                RHI::ColorAttachmentDesc{
                    .TexturePtr = ColorRT,
                    .ClearValue = RHI::ClearColorValue{.R = 0.05f, .G = 0.05f, .B = 0.08f, .A = 1.0f},
                },
        };
        if (!CmdList.PresentSource)
            CmdList.PresentSource = ColorRT;

        auto* DepthRT = Resource::Manager::Get().TryGetReady(View.DepthRT);
        if (!DepthRT) {
            CmdList.Passes.push_back(std::move(Pass));
            return {};
        }

        Pass.Desc.DepthAttachment = RHI::DepthAttachmentDesc{
            .TexturePtr = DepthRT,
            .ClearValue = RHI::ClearDepthStencilValue{.Depth = 1.0f, .Stencil = 0},
        };

        auto* Sampler = Resource::Manager::Get().TryGetReady(m_LinearSampler);
        if (!Sampler) {
            CmdList.Passes.push_back(std::move(Pass));
            return {};
        }

        auto* Pipeline = Resource::Manager::Get().TryGetReady(m_Pipeline);
        if (!Pipeline) {
            CmdList.Passes.push_back(std::move(Pass));
            return {};
        }

        if (!m_Textures) {
            CmdList.Passes.push_back(std::move(Pass));
            return {};
        }
        auto Textures = m_Textures->TryGetReady();
        if (!Textures) {
            CmdList.Passes.push_back(std::move(Pass));
            return {};
        }

        auto* ViewCB = Resource::Manager::Get().TryGetReady(View.ViewCB);
        if (!ViewCB) {
            CmdList.Passes.push_back(std::move(Pass));
            return {};
        }

        auto& Parameters = GetViewParameters(View, *Pipeline);
        if (auto R = Parameters.SetConstantBuffer("g_frameView.frame", FrameCB, &FrameData, sizeof(FrameData)); !R)
            return std::unexpected(R.error().Append("Test frame parameter binding failed"));
        const auto ViewData = View.GetViewConstants();
        if (auto R = Parameters.SetConstantBuffer("g_frameView.view", ViewCB, &ViewData, sizeof(ViewData)); !R)
            return std::unexpected(R.error().Append("Test view parameter binding failed"));
        auto* AnisoSampler = Resource::Manager::Get().TryGetReady(m_AnisoSampler);
        if (auto R = Parameters.SetSampler("g_samplers.uSamplerLinear", Sampler); !R)
            return std::unexpected(R.error().Append("Test linear sampler parameter binding failed"));
        if (auto R = Parameters.SetSampler("g_samplers.uSamplerAniso", AnisoSampler ? AnisoSampler : Sampler); !R)
            return std::unexpected(R.error().Append("Test anisotropic sampler parameter binding failed"));
        if (auto R = Parameters.SetResourceArray("g_textures.uTextures", *Textures); !R)
            return std::unexpected(R.error().Append("Test texture-array parameter binding failed"));

        Pass.SetFullViewport();
        Pass.SetFullScissorRect();
        Pass.SetGraphicsPipeline(Pipeline);
        Pass.BindShaderParameters(Pipeline, Parameters);
        const Uint32 BaseColorTextureIndex = 0;
        Pass.PushConstants(Pipeline, 0, &BaseColorTextureIndex, sizeof(BaseColorTextureIndex));
        for (const auto& Packet : Scene.DrawPackets) {
            auto* PositionVB = Resource::Manager::Get().TryGetReady(Packet.PositionVB);
            auto* NormalVB   = Resource::Manager::Get().TryGetReady(Packet.NormalVB);
            auto* TangentVB  = Resource::Manager::Get().TryGetReady(Packet.TangentVB);
            auto* UVVB       = Resource::Manager::Get().TryGetReady(Packet.UVVB);
            auto* IB         = Resource::Manager::Get().TryGetReady(Packet.IB);
            if (!PositionVB || !NormalVB || !TangentVB || !UVVB || !IB)
                continue;

            Pass.DrawIndexed(
                Pipeline,
                std::array<RHI::VertexBuffer*, RHI::kMaxVertexBufferBindings>{PositionVB, NormalVB, TangentVB, UVVB},
                IB);
        }

        CmdList.Passes.push_back(std::move(Pass));
        return {};
    }

    [[nodiscard]] auto BuildFrameConstants(float Time) const -> FrameConstants {
        return FrameConstants{
            .Time = Time,
        };
    }

    auto GetViewParameters(const Scene::RenderViewSnapshot& View, const RHI::GraphicsPipeline& Pipeline)
        -> RHI::ShaderParameters& {
        const auto& ViewConstantBufferKey = View.ViewCB.GetKey();
        for (auto& State : m_ViewParameters) {
            if (State.ViewConstantBufferKey != ViewConstantBufferKey)
                continue;
            if (State.Parameters.GetLayoutId() != Pipeline.GetShaderParameterLayout().GetId())
                State.Parameters = RHI::ShaderParameters::Create(Pipeline);
            return State.Parameters;
        }

        auto& State = m_ViewParameters.emplace_back(ViewParameterState{
            .ViewConstantBufferKey = ViewConstantBufferKey,
            .Parameters            = RHI::ShaderParameters::Create(Pipeline),
        });
        return State.Parameters;
    }

    Resource::ResourceRef<RHI::GraphicsPipeline> m_Pipeline = {};
    Resource::ResourceRef<RHI::ConstantBuffer>   m_FrameConstants = {};
    Resource::ResourceRef<RHI::Sampler>          m_LinearSampler   = {};
    Resource::ResourceRef<RHI::Sampler>          m_AnisoSampler    = {};
    std::optional<Resource::Array<RHI::SampledTexture>> m_Textures   = std::nullopt;
    std::vector<ViewParameterState>                      m_ViewParameters = {};
};

} // namespace SoulEngine::Renderer
