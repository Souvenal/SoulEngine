export module Renderer:RayTracing.RayTracingRenderer;

import Core;
import EditorTypes;
import RHI;
import Scene;

import :Common;
import :Interface;

import std;

namespace SoulEngine {

/// @brief RenderGraph-driven hardware path-tracing renderer.
///
/// Per frame: the GeometryUploader lazily uploads geometry; every BLAS it requests joins the frame's
/// pending-build AS work alongside a freshly created per-frame TLAS (no culling — secondary rays need
/// off-screen geometry), which the device builds ahead of every pass; PathTracingPass traces into a transient
/// output target and BlitToSwapchainPass presents it directly (no SceneColorRT roundtrip).
export class RayTracingRenderer final : public IRenderer {
  public:
    RayTracingRenderer() = default;
    ~RayTracingRenderer() override;

    [[nodiscard]] auto OnAttach() -> std::expected<void, ErrorMessage> override;

    auto OnDetach() -> void override;

    [[nodiscard]] auto Render(const GameSnapshot& Scene, const EditorSnapshot& Editor)
        -> std::expected<RenderResult, ErrorMessage> override;

  private:
    /// @brief Upload-gate the frame's instances: queue lazy buffer and BLAS
    /// uploads on first encounter, then keep only GPU-ready instances for this frame's tables.
    [[nodiscard]] auto FilterInstances(std::span<const InstanceRecord> SourceInstances) -> std::vector<InstanceRecord>;

    GeometryUploader   m_Uploader      = {};
    RHIRef<RHISampler> m_SamplerLinear = nullptr;
};

} // namespace SoulEngine
