export module RHI:Command;

export import :Types;

export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::RHI {

/// @brief Set the active graphics pipeline for subsequent draw calls.
struct SetPipelineCmd {
    /// Non-owning observer. Producer must keep the pipeline alive until Execute() completes.
    GraphicsPipeline* Pipeline = nullptr;
};

/// @brief Bind a vertex buffer at binding slot 0.
struct BindVertexBufferCmd {
    /// Non-owning observer. Producer must keep the buffer alive until Execute() completes.
    VertexBuffer* Buffer = nullptr;
    Uint64        Offset = 0;
};

/// @brief Bind an index buffer.
struct BindIndexBufferCmd {
    /// Non-owning observer. Producer must keep the buffer alive until Execute() completes.
    IndexBuffer* Buffer = nullptr;
    Uint64       Offset = 0;
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

/// @brief Material data for a draw call — holds bindless texture references.
struct DrawMaterialData {
    /// Non-owning observer. Producer must keep the texture alive until Execute() completes.
    SampledTexture* TestTexture = nullptr;
};

/// @brief Set draw material data (replaces SetSampledTexture).
struct SetDrawMaterialDataCmd {
    DrawMaterialData Material;
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
                             SetDrawMaterialDataCmd>;

/// @brief One rendering pass with attachments and commands inside.
/// Backend automatically wraps each pass with begin/end rendering.
struct Pass {
    RenderingDesc        Desc;
    std::vector<Command> Commands;

    // ── Builder helpers ──────────────────────────────────────────────

    auto SetPipeline(GraphicsPipeline* P) -> void {
        Commands.emplace_back(SetPipelineCmd{P});
    }
    auto BindVertexBuffer(VertexBuffer* Buf, Uint64 Offset = 0) -> void {
        Commands.emplace_back(BindVertexBufferCmd{Buf, Offset});
    }
    auto BindIndexBuffer(IndexBuffer* Buf, Uint64 Offset = 0) -> void {
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
    auto SetDrawMaterialData(const DrawMaterialData& Data) -> void {
        Commands.emplace_back(SetDrawMaterialDataCmd{Data});
    }
};

/// @brief Complete frame's worth of GPU commands, produced by RenderLoop,
/// consumed by RenderDevice::Execute().
struct CommandList {
    std::vector<Pass>      Passes;
    std::vector<std::byte> GlobalConstantData;
    /// Final frame output. Backend presents this engine-owned RT to swapchain.
    RenderTarget* PresentSource = nullptr;
};

} // namespace SoulEngine::RHI
