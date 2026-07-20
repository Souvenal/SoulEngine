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

/// @brief Push a CPU-side byte snapshot into the active graphics pipeline's push-constant range.
struct PushConstantsCmd {
    /// Pipeline expected to be bound when the push happens. Backends use this for
    /// validation and pipeline-layout lookup.
    GraphicsPipeline*        PipelinePtr = nullptr;
    Uint32                   Offset      = 0;
    std::vector<std::byte>   Data        = {};
};

/// @brief Bind a reflection-derived shader parameter snapshot to the active pipeline.
struct BindShaderParametersCmd {
    /// Pipeline expected to be bound when the parameter snapshot is bound.
    GraphicsPipeline* PipelinePtr = nullptr;
    /// Value snapshot keeps per-frame constant data stable until the RHI thread consumes it.
    ShaderParameters  Parameters  = {};
};

/// @brief Draw indexed primitives.
struct DrawIndexedCmd {
    /// Pipeline expected to be bound for this draw. Backends use this for
    /// validation and for lowering draw parameters that require pipeline
    /// layout information, such as Vulkan push constants.
    GraphicsPipeline*                         PipelinePtr     = nullptr;
    std::array<VertexBuffer*, kMaxVertexBufferBindings> VertexBuffers = {};
    IndexBuffer*                              IndexBufferPtr  = nullptr;
};

/// @brief Draw non-indexed primitives.
struct DrawCmd {
    /// Pipeline expected to be bound for this draw. Backends use this for
    /// validation and for lowering draw parameters that require pipeline
    /// layout information, such as Vulkan push constants.
    GraphicsPipeline*                         PipelinePtr     = nullptr;
    std::array<VertexBuffer*, kMaxVertexBufferBindings> VertexBuffers = {};
};

/// @brief All command types dispatched via std::visit.
using Command = std::variant<SetViewportCmd,
                             SetFullViewportCmd,
                             SetScissorCmd,
                             SetFullScissorRectCmd,
                             SetGraphicsPipelineCmd,
                             PushConstantsCmd,
                             BindShaderParametersCmd,
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
    auto PushConstants(GraphicsPipeline* PipelinePtr, Uint32 Offset, const void* Data, Uint64 Size) -> void {
        if (Size == 0)
            return;

        PushConstantsCmd Cmd{
            .PipelinePtr = PipelinePtr,
            .Offset      = Offset,
        };
        Cmd.Data.resize(Size);
        std::memcpy(Cmd.Data.data(), Data, Size);
        Commands.emplace_back(std::move(Cmd));
    }
    auto BindShaderParameters(GraphicsPipeline* PipelinePtr, ShaderParameters Parameters) -> void {
        Commands.emplace_back(BindShaderParametersCmd{
            .PipelinePtr = PipelinePtr,
            .Parameters  = std::move(Parameters),
        });
    }
    auto DrawIndexed(GraphicsPipeline* PipelinePtr,
                     VertexBuffer*     VertexBufferPtr,
                     IndexBuffer*      IndexBufferPtr) -> void {
        DrawIndexed(PipelinePtr, std::array<VertexBuffer*, kMaxVertexBufferBindings>{VertexBufferPtr}, IndexBufferPtr);
    }
    auto DrawIndexed(GraphicsPipeline*                                PipelinePtr,
                     std::array<VertexBuffer*, kMaxVertexBufferBindings> VertexBuffers,
                     IndexBuffer*                                     IndexBufferPtr) -> void {
        Commands.emplace_back(DrawIndexedCmd{
            .PipelinePtr     = PipelinePtr,
            .VertexBuffers   = VertexBuffers,
            .IndexBufferPtr  = IndexBufferPtr,
        });
    }
    auto Draw(GraphicsPipeline* PipelinePtr, VertexBuffer* VertexBufferPtr) -> void {
        Draw(PipelinePtr, std::array<VertexBuffer*, kMaxVertexBufferBindings>{VertexBufferPtr});
    }
    auto Draw(GraphicsPipeline*                                PipelinePtr,
              std::array<VertexBuffer*, kMaxVertexBufferBindings> VertexBuffers) -> void {
        Commands.emplace_back(DrawCmd{
            .PipelinePtr   = PipelinePtr,
            .VertexBuffers = VertexBuffers,
        });
    }
};

/// @brief Complete frame's worth of GPU commands, produced by RenderLoop,
/// consumed by RenderDevice::Execute().
struct CommandList {
    std::vector<Pass> Passes;
    /// Final frame output. Backend presents this engine-owned RT to swapchain.
    RenderTarget*     PresentSource = nullptr;
};

} // namespace SoulEngine::RHI
