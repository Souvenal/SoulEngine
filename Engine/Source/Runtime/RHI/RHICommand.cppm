export module RHI:Command;

export import :Types;

export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::RHI {

/// @brief Set the active graphics pipeline for subsequent draw calls.
struct SetPipelineCmd {
    SPtr<GraphicsPipeline> Pipeline;
};

/// @brief Bind a vertex buffer at binding slot 0.
struct BindVertexBufferCmd {
    SPtr<VertexBuffer> Buffer;
    Uint64             Offset = 0;
};

/// @brief Bind an index buffer.
struct BindIndexBufferCmd {
    SPtr<IndexBuffer> Buffer;
    Uint64            Offset = 0;
};

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

/// @brief Draw indexed primitives.
struct DrawIndexedCmd {
    Uint32 IndexCount    = 0;
    Uint32 InstanceCount = 1;
    Uint32 FirstIndex    = 0;
    Int32  VertexOffset  = 0;
    Uint32 FirstInstance = 0;
};

/// @brief Draw non-indexed primitives.
struct DrawCmd {
    Uint32 VertexCount   = 0;
    Uint32 InstanceCount = 1;
    Uint32 FirstVertex   = 0;
    Uint32 FirstInstance = 0;
};

/// @brief Bind a texture for shader sampling.
struct SetTextureCmd {
    Uint32              Slot       = 0;
    const RHI::Texture* TexturePtr = nullptr;
};

/// @brief All command types dispatched via std::visit.
using Command = std::variant<SetPipelineCmd,
                             BindVertexBufferCmd,
                             BindIndexBufferCmd,
                             SetViewportCmd,
                             SetFullViewportCmd,
                             SetScissorCmd,
                             SetFullScissorRectCmd,
                             DrawIndexedCmd,
                             DrawCmd,
                             SetTextureCmd>;

/// @brief One rendering pass with attachments and commands inside.
/// Backend automatically wraps each pass with begin/end rendering.
struct Pass {
    RenderingDesc        Desc;
    std::vector<Command> Commands;

    // ── Builder helpers ──────────────────────────────────────────────

    auto SetPipeline(SPtr<GraphicsPipeline> P) -> void {
        Commands.emplace_back(SetPipelineCmd{P});
    }
    auto BindVertexBuffer(SPtr<VertexBuffer> Buf, Uint64 Offset = 0) -> void {
        Commands.emplace_back(BindVertexBufferCmd{Buf, Offset});
    }
    auto BindIndexBuffer(SPtr<IndexBuffer> Buf, Uint64 Offset = 0) -> void {
        Commands.emplace_back(BindIndexBufferCmd{Buf, Offset});
    }
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
    auto DrawIndexed(Uint32 IndexCount,
                     Uint32 InstanceCount = 1,
                     Uint32 FirstIndex    = 0,
                     Int32  VertexOffset  = 0,
                     Uint32 FirstInstance = 0) -> void {
        Commands.emplace_back(DrawIndexedCmd{IndexCount, InstanceCount, FirstIndex, VertexOffset, FirstInstance});
    }
    auto Draw(Uint32 VertexCount, Uint32 InstanceCount = 1, Uint32 FirstVertex = 0, Uint32 FirstInstance = 0) -> void {
        Commands.emplace_back(DrawCmd{VertexCount, InstanceCount, FirstVertex, FirstInstance});
    }
    auto SetTexture(Uint32 Slot, const RHI::Texture* Tex) -> void {
        Commands.emplace_back(SetTextureCmd{Slot, Tex});
    }
};

/// @brief Complete frame's worth of GPU commands, produced by RenderLoop,
/// consumed by RenderDevice::Execute().
struct CommandList {
    std::vector<Pass>      Passes;
    std::vector<std::byte> GlobalConstantData;
};

} // namespace SoulEngine::RHI
