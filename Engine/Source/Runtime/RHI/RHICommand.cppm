export module RHI:Command;

export import :Types;

export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::RHI {

/// @brief Set viewport rectangle.
struct SetViewportCmd {
    Float32 X        = 0.0f;
    Float32 Y        = 0.0f;
    Float32 Width    = 0.0f;
    Float32 Height   = 0.0f;
    Float32 MinDepth = 0.0f;
    Float32 MaxDepth = 1.0f;
};

/// @brief Set viewport to the full current pass render target.
struct SetFullViewportCmd {
    Float32 MinDepth = 0.0f;
    Float32 MaxDepth = 1.0f;
};

/// @brief Set scissor rectangle.
struct SetScissorCmd {
    Int32  X      = 0;
    Int32  Y      = 0;
    Uint32 Width  = 0;
    Uint32 Height = 0;
};

/// @brief Set scissor to the full current pass render target.
struct SetFullScissorRectCmd {};

/// @brief Bind the graphics pipeline used by subsequent draw calls.
struct SetGraphicsPipelineCmd {
    /// Non-owning observer. Producer must keep the pipeline alive until Execute() completes.
    GraphicsPipeline* PipelinePtr = nullptr;
};

/// @brief Draw parameters interpreted by the draw call's graphics pipeline.
struct DrawParameter {
    /// Non-owning observer. Producer must keep the texture alive until Execute() completes.
    SampledTexture* TestTexture = nullptr;
};

/// @brief Draw indexed primitives.
struct DrawIndexedCmd {
    /// Pipeline expected to be bound for this draw. Backends use this for
    /// validation and for lowering draw parameters that require pipeline
    /// layout information, such as Vulkan push constants.
    GraphicsPipeline* PipelinePtr     = nullptr;
    VertexBuffer*     VertexBufferPtr = nullptr;
    IndexBuffer*      IndexBufferPtr  = nullptr;
    DrawParameter     Parameters      = {};
};

/// @brief Draw non-indexed primitives.
struct DrawCmd {
    /// Pipeline expected to be bound for this draw. Backends use this for
    /// validation and for lowering draw parameters that require pipeline
    /// layout information, such as Vulkan push constants.
    GraphicsPipeline* PipelinePtr     = nullptr;
    VertexBuffer*     VertexBufferPtr = nullptr;
    DrawParameter     Parameters      = {};
};

/// @brief All command types dispatched via std::visit.
using Command = std::variant<SetViewportCmd,
                             SetFullViewportCmd,
                             SetScissorCmd,
                             SetFullScissorRectCmd,
                             SetGraphicsPipelineCmd,
                             DrawIndexedCmd,
                             DrawCmd>;

/// @brief One rendering pass with attachments and commands inside.
/// Backend automatically wraps each pass with begin/end rendering.
struct Pass {
    RenderingDesc        Desc;
    std::vector<Command> Commands;

    // ── Builder helpers ──────────────────────────────────────────────

    auto SetViewport(Float32 X, Float32 Y, Float32 W, Float32 H, Float32 MinDepth = 0.0f, Float32 MaxDepth = 1.0f)
        -> void {
        Commands.emplace_back(SetViewportCmd{X, Y, W, H, MinDepth, MaxDepth});
    }
    auto SetFullViewport(Float32 MinDepth = 0.0f, Float32 MaxDepth = 1.0f) -> void {
        Commands.emplace_back(SetFullViewportCmd{.MinDepth = MinDepth, .MaxDepth = MaxDepth});
    }
    auto SetScissorRect(Int32 X, Int32 Y, Uint32 W, Uint32 H) -> void {
        Commands.emplace_back(SetScissorCmd{X, Y, W, H});
    }
    auto SetFullScissorRect() -> void {
        Commands.emplace_back(SetFullScissorRectCmd{});
    }
    auto SetGraphicsPipeline(GraphicsPipeline* PipelinePtr) -> void {
        Commands.emplace_back(SetGraphicsPipelineCmd{.PipelinePtr = PipelinePtr});
    }
    auto DrawIndexed(GraphicsPipeline* PipelinePtr,
                     VertexBuffer*     VertexBufferPtr,
                     IndexBuffer*      IndexBufferPtr,
                     DrawParameter     Parameters) -> void {
        Commands.emplace_back(DrawIndexedCmd{.PipelinePtr     = PipelinePtr,
                                             .VertexBufferPtr = VertexBufferPtr,
                                             .IndexBufferPtr  = IndexBufferPtr,
                                             .Parameters      = Parameters});
    }
    auto Draw(GraphicsPipeline* PipelinePtr, VertexBuffer* VertexBufferPtr, DrawParameter Parameters) -> void {
        Commands.emplace_back(
            DrawCmd{.PipelinePtr = PipelinePtr, .VertexBufferPtr = VertexBufferPtr, .Parameters = Parameters});
    }
};

/// @brief Complete frame's worth of GPU commands, produced by RenderLoop,
/// consumed by RenderDevice::Execute().
struct CommandList {
    std::vector<Pass>      Passes;
    std::vector<std::byte> GlobalConstantData;
    /// Final frame output. Backend presents this engine-owned RT to swapchain.
    RenderTarget*          PresentSource = nullptr;
};

} // namespace SoulEngine::RHI
