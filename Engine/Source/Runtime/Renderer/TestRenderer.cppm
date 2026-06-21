module;

#include <glm/glm.hpp>

export module Renderer:TestRenderer;

import Core;
import RHI;
import Scene;
import ShaderCache;

import :IRenderer;

export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Renderer {

struct Vertex {
    glm::vec3 Position;
    glm::vec3 Color;
};

const std::vector<Vertex> kQuadVertices = {
    {.Position = {-0.5f, 0.0f, -0.5f}, .Color = {1.0f, 0.0f, 0.0f}},
    {.Position = {+0.5f, 0.0f, -0.5f}, .Color = {0.0f, 1.0f, 0.0f}},
    {.Position = {+0.5f, 0.0f, +0.5f}, .Color = {0.0f, 0.0f, 1.0f}},
    {.Position = {-0.5f, 0.0f, +0.5f}, .Color = {1.0f, 1.0f, 1.0f}},
};

const std::vector<Uint32> kQuadIndices = {0, 1, 2, 2, 3, 0};

/// @brief Constant buffer layout consumed by Test/CubeGlobalCB.slang.
struct alignas(16) GlobalCBData {
    alignas(16) glm::mat4 View       = glm::mat4(1.0f);
    alignas(16) glm::mat4 Projection = glm::mat4(1.0f);
    alignas(16) float     Time       = 0.0f;
};
static_assert(sizeof(GlobalCBData) == 144, "GlobalCBData must match Test/CubeGlobalCB.slang std140 layout");

/// @brief Minimal prototype renderer — emits CommandList for RHIThread.
class TestRenderer final : public IRenderer {
  public:
    TestRenderer() = default;
    ~TestRenderer() override {
        OnDetach();
    }

    [[nodiscard]] auto OnAttach() -> std::expected<void, ErrorMessage> override {
        auto& Ctx = RHI::RenderDevice::Get();

        auto ShaderDir = ConfigManager::Get().ShadersDirPath() / "Test";
        auto Pass      = GraphicsPass::Create(GraphicsPassDesc{
            .VertEntry = {.ShaderPath = Path((ShaderDir / "CubeGlobalCB.slang").string()), .EntryName = "vertMain"},
            .FragEntry = {.ShaderPath = Path((ShaderDir / "CubeGlobalCB.slang").string()), .EntryName = "fragMain"},
        });
        if (!Pass)
            return std::unexpected(Pass.error().Append("GraphicsPass creation failed"));
        m_Passes.push_back(std::move(*Pass));

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

        return {};
    }

    auto OnDetach() -> void override {
        m_Passes.clear();
        m_VertexBuffer.reset();
        m_IndexBuffer.reset();
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
        Pass.SetFullViewport();
        Pass.SetFullScissorRect();

        for (const auto& GP : m_Passes) {
            auto Pipe = GP.GetPipeline();
            if (!Pipe)
                continue;
            Pass.SetPipeline(Pipe);
            Pass.BindVertexBuffer(m_VertexBuffer);
            Pass.BindIndexBuffer(m_IndexBuffer);
            Pass.DrawIndexed(static_cast<Uint32>(kQuadIndices.size()));
        }

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

    SPtr<RHI::VertexBuffer> m_VertexBuffer;
    SPtr<RHI::IndexBuffer>  m_IndexBuffer;
};

} // namespace SoulEngine::Renderer
