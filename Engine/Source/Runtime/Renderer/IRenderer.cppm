module;

export module Renderer:IRenderer;

import Core;
import RHI;
import Resource;
import Scene;
import TaskGraph;

export import std;

export namespace SoulEngine {

/// @brief Render-thread packet retained by its FrameSlot until GPU completion.
struct RenderResult {
    RenderPassList CmdList = {};
};

/// @brief Abstract base class for all renderers.
///
/// A renderer owns command contexts and asynchronous resource handles
/// that it resolves before emitting commands each frame.
///
/// Deriving from IRenderer lets you define different pipeline types
/// (RasterRenderer, RayTracingRenderer, etc.) while
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
    /// RHI singleton is available via RHIRenderDevice::Get().
    [[nodiscard]] virtual auto OnAttach() -> std::expected<void, ErrorMessage> = 0;

    /// @brief Release all GPU resources and owned objects.
    virtual auto OnDetach() -> void = 0;

    /// @brief Render the scene snapshot and return commands for RHIThread.
    /// Called by RenderLoop.  Must not call BeginFrame/EndFrame.
    [[nodiscard]] virtual auto Render(const SceneSnapshot& Scene)
        -> std::expected<RenderResult, ErrorMessage> = 0;
};

/// @brief Factory type for renderer creation.
///
/// Each renderer implementation auto-registers from its own module partition,
/// so adding a renderer does not require changes to application code.
using RendererFactory = Factory<IRenderer>;

} // namespace SoulEngine
