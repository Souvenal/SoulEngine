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

/// @brief CPU-side vertex matching the VSInput layout in Triangle.slang.
struct Vertex {
    glm::vec2 Position;
    glm::vec3 Color;
};

const std::vector<Vertex> kQuadVertices = {
    {.Position = {-0.5f, -0.5f}, .Color = {1.0f, 0.0f, 0.0f}}, // top-left, red
    {.Position = {+0.5f, -0.5f}, .Color = {0.0f, 1.0f, 0.0f}}, // top-right, green
    {.Position = {+0.5f, +0.5f}, .Color = {0.0f, 0.0f, 1.0f}}, // bottom-right, blue
    {.Position = {-0.5f, +0.5f}, .Color = {1.0f, 1.0f, 1.0f}}, // bottom-left, white
};

const std::vector<Uint16> kQuadIndices = {
    0, 1, 2, // upper-right triangle
    2, 3, 0, // bottom-left triangle
};

/// @brief Minimal prototype renderer used during engine bring-up.
///
/// Owns one command context and a single GraphicsPass that draws a
/// coloured quad.  Pass order is trivial (one pass).
///
/// RHI context is accessed via RHI::RenderDevice::Get() singleton.
class TestRenderer final : public IRenderer {
  public:
    TestRenderer() = default;
    ~TestRenderer() override {
        OnDetach();
    }

    [[nodiscard]] auto OnAttach() -> std::expected<void, ErrorMessage> override {
        auto& Ctx = RHI::RenderDevice::Get();

        // ── Command context (owned by RenderDevice) ────────────────────
        m_GraphicsCmdList = &Ctx.GetCommandList();

        // ── Graphics passes ─────────────────────────────────────────────
        auto ShaderDir = ConfigManager::Get().ShadersDirPath() / "Test";
        auto Pass      = GraphicsPass::Create(
            GraphicsPassDesc{
                .VertEntry = {.ShaderPath = Path((ShaderDir / "Triangle.slang").string()), .EntryName = "vertMain"},
                .FragEntry = {.ShaderPath = Path((ShaderDir / "Triangle.slang").string()), .EntryName = "fragMain"},
            });
        if (!Pass)
            return std::unexpected(Pass.error().Append("GraphicsPass creation failed"));
        m_Passes.push_back(std::move(*Pass));

        // ── Vertex buffer ──────────────────────────────────────────────
        auto VBO = Ctx.CreateVertexBuffer(kQuadVertices.data(),
                                          kQuadVertices.size(),
                                          sizeof(Vertex));
        if (!VBO)
            return std::unexpected(VBO.error().Append("Vertex buffer creation failed"));

        m_VertexBuffer = std::move(*VBO);

        // ── Index buffer ───────────────────────────────────────────────
        auto IBO = Ctx.CreateIndexBuffer(kQuadIndices.data(),
                                         kQuadIndices.size(),
                                         true); // Uint16
        if (!IBO)
            return std::unexpected(IBO.error().Append("Index buffer creation failed"));

        m_IndexBuffer = std::move(*IBO);

        return {};
    }

    auto OnDetach() -> void override {
        m_GraphicsCmdList = nullptr;
        m_Passes.clear();
    }

    /// Executes passes sequentially, then submits and presents.
    [[nodiscard]] auto Render(const Scene::Scene& /*Scene*/) -> std::expected<void, ErrorMessage> override {
        auto& Ctx = RHI::RenderDevice::Get();

        if (!m_GraphicsCmdList)
            return std::unexpected(ErrorMessage("Renderer is not initialised"));

        if (auto R = Ctx.BeginFrame(); !R)
            return std::unexpected(R.error().Append("BeginFrame failed"));

        auto* CmdList = m_GraphicsCmdList;

        if (auto R = CmdList->Reset(); !R)
            return std::unexpected(R.error().Append("Reset failed"));
        if (auto R = CmdList->Begin(); !R)
            return std::unexpected(R.error().Append("Begin failed"));

        RHI::RenderingDesc RenderingDesc{
            .ColorAttachment =
                RHI::ColorAttachmentDesc{
                    .ClearValue =
                        RHI::ClearColorValue{
                            .R = 0.05f,
                            .G = 0.05f,
                            .B = 0.08f,
                            .A = 1.0f,
                        },
                },
        };

        if (auto R = CmdList->BeginRendering(RenderingDesc); !R)
            return std::unexpected(R.error().Append("BeginRendering failed"));

        // ── Execute passes ──────────────────────────────────────────────
        for (const auto& Pass : m_Passes) {
            const auto& Pipe = Pass.GetPipeline();
            if (!Pipe)
                return std::unexpected(ErrorMessage("Pass pipeline is not initialised"));

            if (auto R = CmdList->BindPipeline(*Pipe); !R)
                return std::unexpected(R.error().Append("BindPipeline failed"));
            if (auto R = CmdList->BindVertexBuffer(*m_VertexBuffer); !R)
                return std::unexpected(R.error().Append("BindVertexBuffer failed"));
            if (auto R = CmdList->BindIndexBuffer(m_IndexBuffer); !R)
                return std::unexpected(R.error().Append("BindIndexBuffer failed"));
            if (auto R = CmdList->DrawIndexed(static_cast<Uint32>(kQuadIndices.size())); !R)
                return std::unexpected(R.error().Append("DrawIndexed failed"));
        }

        if (auto R = CmdList->EndRendering(); !R)
            return std::unexpected(R.error().Append("EndRendering failed"));
        if (auto R = CmdList->End(); !R)
            return std::unexpected(R.error().Append("End failed"));

        std::array<RHI::CommandList*, 1> CmdLists{CmdList};
        if (auto R = Ctx.EndFrame(CmdLists); !R)
            return std::unexpected(R.error().Append("EndFrame failed"));

        return {};
    }

  private:
    RHI::CommandList*         m_GraphicsCmdList = nullptr;
    SPtr<RHI::VertexBuffer>   m_VertexBuffer;
    SPtr<RHI::IndexBuffer>    m_IndexBuffer;
};

} // namespace SoulEngine::Renderer
