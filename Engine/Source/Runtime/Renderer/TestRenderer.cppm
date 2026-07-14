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

struct Vertex {
    hlslpp::interop::float3 Position;
    hlslpp::interop::float3 Color;
    hlslpp::interop::float2 UV;
};
static_assert(sizeof(Vertex) == 32, "TestRenderer vertex layout: 2x float3(12) + float2(8) = 32");
static_assert(offsetof(Vertex, Position) == 0, "TestRenderer vertex position offset changed");
static_assert(offsetof(Vertex, Color) == 12, "TestRenderer vertex color offset changed");
static_assert(offsetof(Vertex, UV) == 24, "TestRenderer vertex uv offset changed");

const std::vector<Vertex> kQuadVertices = {
    {.Position = hlslpp::float3(-0.5f, 0.0f, -0.5f),
     .Color    = hlslpp::float3(1.0f, 0.0f, 0.0f),
     .UV       = hlslpp::float2(0.0f, 1.0f)},
    {.Position = hlslpp::float3(+0.5f, 0.0f, -0.5f),
     .Color    = hlslpp::float3(0.0f, 1.0f, 0.0f),
     .UV       = hlslpp::float2(1.0f, 1.0f)},
    {.Position = hlslpp::float3(+0.5f, 0.0f, +0.5f),
     .Color    = hlslpp::float3(0.0f, 0.0f, 1.0f),
     .UV       = hlslpp::float2(1.0f, 0.0f)},
    {.Position = hlslpp::float3(-0.5f, 0.0f, +0.5f),
     .Color    = hlslpp::float3(1.0f, 1.0f, 1.0f),
     .UV       = hlslpp::float2(0.0f, 0.0f)},
};

const std::vector<Uint32> kQuadIndices = {0, 1, 2, 2, 3, 0};

/// @brief Constant buffer layout matching Common.slang FrameData.
struct alignas(16) GlobalCBData {
    alignas(16) hlslpp::float4x4 View       = hlslpp::float4x4::identity();
    alignas(16) hlslpp::float4x4 Projection = hlslpp::float4x4::identity();
    alignas(16) float Time                  = 0.0f;
};
static_assert(sizeof(GlobalCBData) == 144, "GlobalCBData must match Common.slang FrameData std140 layout");

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
                                             .Binding = 0,
                                             .Stride  = sizeof(Vertex),
                                             .Attributes =
                                                 {
                                                     RHI::VertexInputAttributeDesc{
                                                         .Location = 0,
                                                         .Format   = RHI::Format::R32G32B32_SFLOAT,
                                                         .Offset   = offsetof(Vertex, Position),
                                                     },
                                                     RHI::VertexInputAttributeDesc{
                                                         .Location = 1,
                                                         .Format   = RHI::Format::R32G32B32_SFLOAT,
                                                         .Offset   = offsetof(Vertex, Color),
                                                     },
                                                     RHI::VertexInputAttributeDesc{
                                                         .Location = 2,
                                                         .Format   = RHI::Format::R32G32_SFLOAT,
                                                         .Offset   = offsetof(Vertex, UV),
                                                     },
                                                 },
                                         },
                                     .DepthFormat = RHI::Format::D32_SFLOAT,
                                 });
        if (!m_Pipeline)
            return std::unexpected(ErrorMessage("Graphics pipeline request failed"));

        m_VertexBuffer = Resource::Manager::Get().RequestVertexBufferRef(
            "quad_verts",
            RHI::VertexBufferDesc{
                .Data = kQuadVertices.data(), .VertexCount = kQuadVertices.size(), .Stride = sizeof(Vertex)});
        if (!m_VertexBuffer)
            return std::unexpected(ErrorMessage("Vertex buffer request failed"));

        m_IndexBuffer = Resource::Manager::Get().RequestIndexBufferRef(
            "quad_indices", RHI::IndexBufferDesc{.Data = kQuadIndices.data(), .IndexCount = kQuadIndices.size()});
        if (!m_IndexBuffer)
            return std::unexpected(ErrorMessage("Index buffer request failed"));

        m_FrameConstants =
            Resource::Manager::Get().RequestConstantBufferRef("frame_constants", {.Size = sizeof(GlobalCBData)});
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
        m_Pipeline     = {};
        m_VertexBuffer = {};
        m_IndexBuffer  = {};
        m_FrameConstants = {};
        m_LinearSampler  = {};
        m_AnisoSampler   = {};
        m_Textures.reset();
        m_Parameters.reset();
    }

    /// Produce a CommandList from the current scene snapshot.
    /// Called by RenderLoop. Does NOT call BeginFrame/EndFrame.
    [[nodiscard]] auto Render(const Scene::SceneSnapshot& Scene) -> std::expected<RenderResult, ErrorMessage> override {
        auto CbData = BuildGlobalCB(Scene);

        RenderResult Result;

        // Build pass — backend wraps each Pass in begin/end rendering.
        auto* ColorRT = Resource::Manager::Get().TryGetReady(Scene.ColorRT);
        if (!ColorRT)
            return Result;

        RHI::Pass Pass;
        Pass.Desc = RHI::RenderingDesc{
            .ColorAttachment =
                RHI::ColorAttachmentDesc{
                    .TexturePtr = ColorRT,
                    .ClearValue = RHI::ClearColorValue{.R = 0.05f, .G = 0.05f, .B = 0.08f, .A = 1.0f},
                },
        };
        Result.CmdList.PresentSource = ColorRT;

        auto* DepthRT = Resource::Manager::Get().TryGetReady(Scene.DepthRT);
        if (!DepthRT) {
            Result.CmdList.Passes.push_back(std::move(Pass));
            return Result;
        }

        Pass.Desc.DepthAttachment = RHI::DepthAttachmentDesc{
            .TexturePtr = DepthRT,
            .ClearValue = RHI::ClearDepthStencilValue{.Depth = 1.0f, .Stencil = 0},
        };

        auto* Sampler = Resource::Manager::Get().TryGetReady(m_LinearSampler);
        if (!Sampler) {
            Result.CmdList.Passes.push_back(std::move(Pass));
            return Result;
        }

        auto* Pipeline = Resource::Manager::Get().TryGetReady(m_Pipeline);
        if (!Pipeline) {
            Result.CmdList.Passes.push_back(std::move(Pass));
            return Result;
        }

        auto* VB = Resource::Manager::Get().TryGetReady(m_VertexBuffer);
        auto* IB = Resource::Manager::Get().TryGetReady(m_IndexBuffer);
        if (!VB || !IB) {
            Result.CmdList.Passes.push_back(std::move(Pass));
            return Result;
        }

        if (!m_Textures) {
            Result.CmdList.Passes.push_back(std::move(Pass));
            return Result;
        }
        auto Textures = m_Textures->TryGetReady();
        if (!Textures) {
            Result.CmdList.Passes.push_back(std::move(Pass));
            return Result;
        }

        if (!m_Parameters)
            m_Parameters = RHI::ShaderParameters::Create(*Pipeline);

        if (auto* FrameCB = Resource::Manager::Get().TryGetReady(m_FrameConstants)) {
            if (auto R = m_Parameters->SetConstantBuffer("g_frame.cb", FrameCB, &CbData, sizeof(CbData)); !R)
                return std::unexpected(R.error().Append("Test frame parameter binding failed"));
        }
        auto* AnisoSampler = Resource::Manager::Get().TryGetReady(m_AnisoSampler);
        if (auto R = m_Parameters->SetSampler("g_samplers.uSamplerLinear", Sampler); !R)
            return std::unexpected(R.error().Append("Test linear sampler parameter binding failed"));
        if (auto R = m_Parameters->SetSampler("g_samplers.uSamplerAniso", AnisoSampler ? AnisoSampler : Sampler); !R)
            return std::unexpected(R.error().Append("Test anisotropic sampler parameter binding failed"));
        if (auto R = m_Parameters->SetResourceArray("g_textures.uTextures", *Textures); !R)
            return std::unexpected(R.error().Append("Test texture-array parameter binding failed"));

        Pass.SetFullViewport();
        Pass.SetFullScissorRect();
        Pass.SetGraphicsPipeline(Pipeline);
        Pass.BindShaderParameters(Pipeline, *m_Parameters);
        const Uint32 BaseColorTextureIndex = 0;
        Pass.PushConstants(Pipeline, 0, &BaseColorTextureIndex, sizeof(BaseColorTextureIndex));
        Pass.DrawIndexed(Pipeline, VB, IB);

        Result.CmdList.Passes.push_back(std::move(Pass));

        return Result;
    }

  private:
    [[nodiscard]] auto BuildGlobalCB(const Scene::SceneSnapshot& Scene) const -> GlobalCBData {
        return GlobalCBData{
            .View       = Scene.GetViewMatrix(),
            .Projection = Scene.GetProjectionMatrix(),
            .Time       = Scene.Time,
        };
    }

    Resource::ResourceRef<RHI::VertexBuffer>     m_VertexBuffer;
    Resource::ResourceRef<RHI::IndexBuffer>      m_IndexBuffer;
    Resource::ResourceRef<RHI::GraphicsPipeline> m_Pipeline = {};
    Resource::ResourceRef<RHI::ConstantBuffer>   m_FrameConstants = {};
    Resource::ResourceRef<RHI::Sampler>          m_LinearSampler   = {};
    Resource::ResourceRef<RHI::Sampler>          m_AnisoSampler    = {};
    std::optional<Resource::Array<RHI::SampledTexture>> m_Textures   = std::nullopt;
    std::optional<RHI::ShaderParameters>                m_Parameters = std::nullopt;
};

} // namespace SoulEngine::Renderer
