module;

export module RenderGraph.Runtime;

import Core;
import RenderGraph.PipelineRegistry;
import RenderGraph.TexturePool;

namespace SoulEngine {

/// @brief Module-level RenderGraph singleton.
///
/// Owns the cross-frame facilities: the transient texture pool and the PipelineRegistry lifecycle. Per-frame
/// graphs are RenderGraphBuilder objects; they never hold cross-frame state.
///
/// Lifecycle is owned by Launch: Init() before SelectRenderer, Tick() once per frame in RenderLoop before
/// IRenderer::Render(), Shutdown() before RHIRenderDevice::Destroy().
export class RenderGraph final : public Singleton<RenderGraph> {
    friend class Singleton<RenderGraph>;

    // Lifecycle (Launch calls these explicitly)
  public:
    /// Clear and reset cross-frame state (idempotent; call before SelectRenderer).
    auto Init() -> void;

    /// Per-frame pool maintenance (LRU eviction). Call in RenderLoop before IRenderer::Render().
    auto Tick() -> void;

    /// @brief Release the pool and all cross-frame pipeline refs.
    ///
    /// Must run before RHIRenderDevice::Destroy() — payload destruction needs a live queue.
    auto Shutdown() -> void;

    /// @brief The transient texture pool builders acquire from.
    [[nodiscard]] auto GetPool() -> RGTexturePool&;

  private:
    RenderGraph() = default;

    RGTexturePool m_Pool = {};
};

} // namespace SoulEngine
