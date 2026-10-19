module RenderGraph:Runtime.impl;

import :Runtime;
import Core;
import :PipelineRegistry;
import :TexturePool;

namespace SoulEngine {

auto RenderGraph::Init() -> void {
    PipelineRegistry::Get().Init();
    m_Pool.Clear();
    m_Pool.Activate();
}

auto RenderGraph::Tick() -> void {
    m_Pool.Tick();
}

auto RenderGraph::Shutdown() -> void {
    m_Pool.Shutdown();
    PipelineRegistry::Get().Clear();
}

[[nodiscard]] auto RenderGraph::GetPool() -> RGTexturePool& {
    return m_Pool;
}

} // namespace SoulEngine
