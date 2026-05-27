export module RHI:CommandList;

export import Core;
export import :Types;

using namespace SoulEngine::Core;

export namespace SoulEngine::RHI {

class CommandList {
  public:
    virtual ~CommandList() = default;

    CommandList()                                      = default;
    CommandList(const CommandList&)                    = delete;
    auto operator=(const CommandList&) -> CommandList& = delete;
    CommandList(CommandList&&)                         = delete;
    auto operator=(CommandList&&) -> CommandList&      = delete;

    // ── Recording lifecycle ──────────────────────────────────────────────────
    //
    // Begin starts command buffer recording.
    // End finishes recording so the buffer can be submitted.
    // Reset returns the command buffer to the initial state after recording.

    [[nodiscard]] virtual auto Begin() -> std::expected<void, ErrorMessage> = 0;
    [[nodiscard]] virtual auto End() -> std::expected<void, ErrorMessage>   = 0;
    [[nodiscard]] virtual auto Reset() -> std::expected<void, ErrorMessage> = 0;

    // ── Rendering scope ──────────────────────────────────────────────────────

    [[nodiscard]] virtual auto BeginRendering(const RenderingDesc& Desc) -> std::expected<void, ErrorMessage> = 0;
    [[nodiscard]] virtual auto EndRendering() -> std::expected<void, ErrorMessage>                            = 0;

    // ── Pipeline binding ────────────────────────────────────────────────────

    [[nodiscard]] virtual auto BindPipeline(const GraphicsPipeline& Pipe) -> std::expected<void, ErrorMessage> = 0;

    // ── Vertex / index buffers ──────────────────────────────────────────────

    [[nodiscard]] virtual auto BindVertexBuffer(const VertexBuffer& VB, Uint64 Offset = 0)
        -> std::expected<void, ErrorMessage> = 0;
    [[nodiscard]] virtual auto BindIndexBuffer(SPtr<IndexBuffer> IdxBuf, Uint64 Offset = 0)
        -> std::expected<void, ErrorMessage> = 0;

    // ── Scissor rect ───────────────────────────────────────────────────────

    [[nodiscard]] virtual auto SetScissorRect(Int32 X, Int32 Y, Uint32 Width, Uint32 Height)
        -> std::expected<void, ErrorMessage> = 0;

    // ── Draw / dispatch ─────────────────────────────────────────────────────

    [[nodiscard]] virtual auto
    Draw(Uint32 VertexCount, Uint32 InstanceCount = 1, Uint32 FirstVertex = 0, Uint32 FirstInstance = 0)
        -> std::expected<void, ErrorMessage>                                                              = 0;
    [[nodiscard]] virtual auto DrawIndexed(Uint32 IndexCount,
                                           Uint32 InstanceCount = 1,
                                           Uint32 FirstIndex    = 0,
                                           Int32  VertexOffset  = 0,
                                           Uint32 FirstInstance = 0) -> std::expected<void, ErrorMessage> = 0;
    [[nodiscard]] virtual auto Dispatch(Uint32 GroupX, Uint32 GroupY, Uint32 GroupZ)
        -> std::expected<void, ErrorMessage> = 0;

    // ── Barriers ────────────────────────────────────────────────────────────

    // TODO: Add TransitionImage / TransitionBuffer pure virtual when a
    // second backend is introduced.  For now, BeginRendering/EndRendering
    // handle the swapchain transitions internally via the Vulkan backend.
};

} // namespace SoulEngine::RHI
