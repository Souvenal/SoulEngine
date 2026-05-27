export module Renderer:GraphicsPass;

import Core;
import RHI;
import Shader;
import ShaderCache;

export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Renderer {

/// @brief Describes a single shader entry point (source file + entry function).
struct ShaderEntry {
    Path   ShaderPath; ///< Path to the .slang source file
    String EntryName;  ///< Name of the entry function
};

/// @brief Description for creating a GraphicsPass.
struct GraphicsPassDesc {
    ShaderEntry VertEntry; ///< Vertex shader entry
    ShaderEntry FragEntry; ///< Fragment shader entry
};

/// @brief A graphics pipeline assembled from shader source and pipeline state.
///
/// Created via the static factory Create(...) which delegates shader compilation
/// to ShaderCache and pipeline creation to RHI::RenderDevice::Get().
///
/// The pass is stateless and does NOT own the pipeline resource — the caller
/// (typically Renderer) is responsible for calling
/// RenderDevice::DestroyPipeline() during shutdown.
class GraphicsPass {
  public:
    GraphicsPass() = default;

    /// Move-only
    GraphicsPass(GraphicsPass&&) noexcept                    = default;
    auto operator=(GraphicsPass&&) noexcept -> GraphicsPass& = default;

    GraphicsPass(const GraphicsPass&)                    = delete;
    auto operator=(const GraphicsPass&) -> GraphicsPass& = delete;

    /// @brief Create a fully-configured graphics pipeline from a description.
    ///
    /// Steps:
    ///   1. Compile/reuse vertex shader via ShaderCache
    ///   2. Compile/reuse fragment shader via ShaderCache
    ///   3. Assemble a GraphicsPipelineDesc (all state fields use defaults)
    ///   4. Create the Vulkan pipeline via RHI::RenderDevice
    [[nodiscard]] static auto Create(const GraphicsPassDesc& Desc)
        -> std::expected<GraphicsPass, ErrorMessage> {
        namespace SCC = SoulEngine::ShaderCache;

        // ── 1. Vertex shader ──────────────────────────────────────────────
        auto Vert = SCC::ShaderCache::Get().GetOrCompile(SCC::ShaderCacheRequest{
            .SourcePath = Desc.VertEntry.ShaderPath,
            .EntryPoint = Desc.VertEntry.EntryName,
        });
        if (!Vert)
            return std::unexpected(Vert.error().Append(Format("GraphicsPass: vertex shader '{}'/'{}'",
                                                              Desc.VertEntry.ShaderPath.string(),
                                                              Desc.VertEntry.EntryName)));

        // ── 2. Fragment shader ────────────────────────────────────────────
        auto Frag = SCC::ShaderCache::Get().GetOrCompile(SCC::ShaderCacheRequest{
            .SourcePath = Desc.FragEntry.ShaderPath,
            .EntryPoint = Desc.FragEntry.EntryName,
        });
        if (!Frag)
            return std::unexpected(Frag.error().Append(Format("GraphicsPass: fragment shader '{}'/'{}'",
                                                              Desc.FragEntry.ShaderPath.string(),
                                                              Desc.FragEntry.EntryName)));

        // ── 3. Assemble pipeline desc (all state fields use defaults) ─────
        RHI::GraphicsPipelineDesc PipeDesc {
            .VertexProgram   = std::move(*Vert),
            .FragmentProgram = std::move(*Frag),
        };
        // Topology, Rasterizer, Blend, DepthStencil, ColorFormat, DepthFormat
        // all keep their default values.

        // ── 4. Create GPU pipeline ────────────────────────────────────────
        auto Pipe = RHI::RenderDevice::Get().CreateGraphicsPipeline(PipeDesc);
        if (!Pipe)
            return std::unexpected(Pipe.error().Append("GraphicsPass: CreateGraphicsPipeline failed"));

        return GraphicsPass(std::move(*Pipe));
    }

    [[nodiscard]] auto GetPipeline() const noexcept -> SPtr<RHI::GraphicsPipeline> {
        return m_Pipeline;
    }

  private:
    explicit GraphicsPass(SPtr<RHI::GraphicsPipeline> Pipeline) : m_Pipeline(std::move(Pipeline)) {}

    SPtr<RHI::GraphicsPipeline> m_Pipeline = nullptr;
};

} // namespace SoulEngine::Renderer
