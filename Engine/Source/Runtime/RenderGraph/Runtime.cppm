module;

export module RenderGraph:Runtime;

import Core;
import :PipelineRegistry;
import :RenderTargetPool;

export import std;

export namespace SoulEngine {

/// @brief Module-level RenderGraph singleton (ADR 05).
///
/// Owns the cross-frame facilities: the transient render-target pool and the
/// PipelineRegistry lifecycle. Per-frame graphs are RenderGraphBuilder
/// objects; they never hold cross-frame state.
///
/// Lifecycle is owned by Launch: Init() before SelectRenderer, Tick() once per
/// frame in RenderLoop before IRenderer::Render(), Shutdown() before
/// RHIRenderDevice::Destroy().
class RenderGraph final : public Singleton<RenderGraph> {
    friend class Singleton<RenderGraph>;

  public:
    // ── Lifecycle (Launch calls these explicitly) ───────────────────────

    /// Clear and reset cross-frame state (idempotent; call before SelectRenderer).
    auto Init() -> void {
        PipelineRegistry::Get().Init();
        m_Pool.Clear();
        m_Pool.Activate();
    }

    /// Per-frame pool maintenance (LRU eviction). Call in RenderLoop before
    /// IRenderer::Render().
    auto Tick() -> void {
        m_Pool.Tick();
    }

    /// Release the pool and all cross-frame pipeline refs. Must run before
    /// RHIRenderDevice::Destroy() — payload destruction needs a live queue.
    auto Shutdown() -> void {
        m_Pool.Shutdown();
        PipelineRegistry::Get().Clear();
    }

    /// @brief The transient render-target pool builders acquire from.
    [[nodiscard]] auto GetPool() -> RGRenderTargetPool& {
        return m_Pool;
    }

  private:
    RenderGraph() = default;

    RGRenderTargetPool m_Pool = {};
};

} // namespace SoulEngine
