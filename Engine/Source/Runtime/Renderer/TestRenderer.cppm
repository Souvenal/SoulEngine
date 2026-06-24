module;

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
    hlslpp::float3 Position;
    hlslpp::float3 Color;
};
static_assert(sizeof(Vertex) == 32, "TestRenderer vertex layout expects hlslpp::float3 to occupy 16 bytes");
static_assert(offsetof(Vertex, Position) == 0, "TestRenderer vertex position offset changed");
static_assert(offsetof(Vertex, Color) == 16, "TestRenderer vertex color offset changed");

const std::vector<Vertex> kQuadVertices = {
    {.Position = {-0.5f, 0.0f, -0.5f}, .Color = {1.0f, 0.0f, 0.0f}},
    {.Position = {+0.5f, 0.0f, -0.5f}, .Color = {0.0f, 1.0f, 0.0f}},
    {.Position = {+0.5f, 0.0f, +0.5f}, .Color = {0.0f, 0.0f, 1.0f}},
    {.Position = {-0.5f, 0.0f, +0.5f}, .Color = {1.0f, 1.0f, 1.0f}},
};

const std::vector<Uint32> kQuadIndices = {0, 1, 2, 2, 3, 0};

/// @brief Constant buffer layout matching Common.slang FrameData.
struct alignas(16) GlobalCBData {
    alignas(16) hlslpp::float4x4 View       = hlslpp::float4x4::identity();
    alignas(16) hlslpp::float4x4 Projection = hlslpp::float4x4::identity();
    alignas(16) float Time                  = 0.0f;
};
static_assert(sizeof(GlobalCBData) == 144, "GlobalCBData must match Common.slang FrameData std140 layout");

/// @brief Minimal prototype renderer — emits CommandList for RHIThread.
class TestRenderer final : public IRenderer {
  public:
    TestRenderer() = default;
    ~TestRenderer() override {
        OnDetach();
    }

    [[nodiscard]] auto OnAttach() -> std::expected<void, ErrorMessage> override {
        auto& Ctx = RHI::RenderDevice::Get();

        auto ShaderDir = ConfigManager::Get().CurrentApplicationDir() / "Shaders";
        m_Pipeline     = Resource::Manager::Get().RequestGraphicsPipeline(Resource::GraphicsPipelineRequest{
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
                        },
                },
        });
        if (!m_Pipeline.IsValid())
            return std::unexpected(ErrorMessage("Graphics pipeline request failed"));

        auto VBO = Ctx.CreateVertexBuffer(RHI::VertexBufferDesc{
            .Data = kQuadVertices.data(), .VertexCount = kQuadVertices.size(), .Stride = sizeof(Vertex)});
        if (!VBO)
            return std::unexpected(VBO.error().Append("Vertex buffer creation failed"));
        m_VertexBuffer = std::move(*VBO);

        auto IBO =
            Ctx.CreateIndexBuffer(RHI::IndexBufferDesc{.Data = kQuadIndices.data(), .IndexCount = kQuadIndices.size()});
        if (!IBO)
            return std::unexpected(IBO.error().Append("Index buffer creation failed"));
        m_IndexBuffer = std::move(*IBO);

        m_Texture = Resource::Manager::Get().RequestSampledTexture(
            (ConfigManager::Get().CurrentApplicationDir() / "Assets" / "statue.jpg").string());
        if (!m_Texture.IsValid())
            return std::unexpected(ErrorMessage("Test texture request failed"));

        return {};
    }

    auto OnDetach() -> void override {
        m_Pipeline = {};
        m_VertexBuffer.reset();
        m_IndexBuffer.reset();
        m_Texture = {};
    }

    /// Produce a CommandList from the current scene data.
    /// Called by RenderLoop. Does NOT call BeginFrame/EndFrame.
    [[nodiscard]] auto Render(const Scene::Scene& Scene) -> std::expected<RHI::CommandList, ErrorMessage> override {
        auto CbData = BuildGlobalCB(Scene);

        // Copy into command list
        RHI::CommandList CmdList;
        CmdList.GlobalConstantData.resize(sizeof(CbData));
        std::memcpy(CmdList.GlobalConstantData.data(), &CbData, sizeof(CbData));

        // Build pass — backend wraps each Pass in begin/end rendering.
        RHI::Pass Pass;
        Pass.Desc = RHI::RenderingDesc{
            .ColorAttachment =
                RHI::ColorAttachmentDesc{
                    .ClearValue = RHI::ClearColorValue{.R = 0.05f, .G = 0.05f, .B = 0.08f, .A = 1.0f},
                },
        };

        auto Texture = m_Texture.TryGet();
        if (!Texture || !Texture->Texture) {
            CmdList.Passes.push_back(std::move(Pass));
            return CmdList;
        }

        auto Pipeline = m_Pipeline.TryGet();
        if (!Pipeline || !Pipeline->Pipeline) {
            CmdList.Passes.push_back(std::move(Pass));
            return CmdList;
        }

        Pass.SetFullViewport();
        Pass.SetFullScissorRect();
        Pass.SetPipeline(Pipeline->Pipeline);
        Pass.BindVertexBuffer(m_VertexBuffer);
        Pass.BindIndexBuffer(m_IndexBuffer);
        Pass.SetSampledTexture(0, Texture->Texture.get());
        Pass.DrawIndexed(static_cast<Uint32>(kQuadIndices.size()));

        CmdList.Passes.push_back(std::move(Pass));

        return CmdList;
    }

  private:
    [[nodiscard]] auto BuildGlobalCB(const Scene::Scene& Scene) const -> GlobalCBData {
        const auto& Camera = Scene.m_Camera;

        return GlobalCBData{
            .View       = Camera.GetViewMatrix(),
            .Projection = Camera.GetProjectionMatrix(),
            .Time       = Scene.m_Time,
        };
    }

    SPtr<RHI::VertexBuffer>          m_VertexBuffer;
    SPtr<RHI::IndexBuffer>           m_IndexBuffer;
    Resource::GraphicsPipelineHandle m_Pipeline = {};
    Resource::SampledTextureHandle   m_Texture  = {};
};

} // namespace SoulEngine::Renderer
