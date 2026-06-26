module;

export module Renderer:IRenderer;

import Core;
import RHI;
import Resource;
import Scene;

export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Renderer {

/// @brief Abstract base class for all renderers.
///
/// A renderer owns command contexts and asynchronous resource handles
/// that it resolves before emitting commands each frame.
///
/// Deriving from IRenderer lets you define different pipeline types
/// (ForwardRenderer, DeferredRenderer, RayTracingRenderer, etc.) while
/// sharing the per-frame execution loop and pass management.
class IRenderer {
  public:
    IRenderer() = default;

    IRenderer(const IRenderer&)                    = delete;
    auto operator=(const IRenderer&) -> IRenderer& = delete;
    IRenderer(IRenderer&&)                         = delete;
    auto operator=(IRenderer&&) -> IRenderer&      = delete;

    virtual ~IRenderer() = default;

    /// @brief Create GPU resources (passes, buffers, etc.).
    /// RHI singleton is available via RHI::RenderDevice::Get().
    [[nodiscard]] virtual auto OnAttach() -> std::expected<void, ErrorMessage> = 0;

    /// @brief Release all GPU resources and owned objects.
    virtual auto OnDetach() -> void = 0;

    /// @brief Render the scene snapshot and return a CommandList for RHIThread.
    /// Called by RenderLoop.  Must not call BeginFrame/EndFrame.
    [[nodiscard]] virtual auto Render(const Scene::SceneSnapshot& Scene) -> std::expected<RHI::CommandList, ErrorMessage> = 0;
};

} // namespace SoulEngine::Renderer
