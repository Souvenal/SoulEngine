module;

export module RHI:Pass;

import Core;
import :Command;

export import std;

export namespace SoulEngine {

enum class RHIPassType : Uint8 {
    Unknown = 0,
    Graphics,
    RayTracing,
};

/// @brief Renderer-independent pass builder and final RHI execution packet.
class IRHIPass {
  public:
    IRHIPass()                                             = default;
    IRHIPass(const IRHIPass&)                              = delete;
    auto operator=(const IRHIPass&) -> IRHIPass&           = delete;
    IRHIPass(IRHIPass&&)                                   = default;
    auto operator=(IRHIPass&&) -> IRHIPass&                = default;
    virtual ~IRHIPass()                                    = default;

    [[nodiscard]] virtual auto Record() -> std::expected<void, ErrorMessage> = 0;
    [[nodiscard]] virtual auto GetType() const noexcept -> RHIPassType       = 0;

    [[nodiscard]] auto GetPipeline() const noexcept -> const RHIPipeline& {
        return m_Pipeline;
    }

    [[nodiscard]] auto GetCommands() const noexcept -> std::span<const RHICommand> {
        return m_Commands;
    }

  protected:
    explicit IRHIPass(RHIPipeline Pipeline) : m_Pipeline(std::move(Pipeline)) {}

    template <typename T>
    [[nodiscard]] auto BindResource(StringView ResourceNameInShader,
                                    RHIRef<T>   Resource,
                                    bool        IsReadOnly) -> std::expected<void, ErrorMessage> {
        auto* BindingSet = GetShaderBindingSet(m_Pipeline);
        if (!BindingSet)
            return std::unexpected(
                ErrorMessage(Format("Pass has no pipeline shader binding set for '{}'", ResourceNameInShader)));
        return BindingSet->BindResource(ResourceNameInShader, std::move(Resource), IsReadOnly);
    }

    template <typename T>
    [[nodiscard]] auto BindResource(RHIRefArray<T> Resource) -> std::expected<void, ErrorMessage> {
        auto* BindingSet = GetShaderBindingSet(m_Pipeline);
        if (!BindingSet)
            return std::unexpected(ErrorMessage("Pass has no pipeline shader binding set"));
        return BindingSet->BindResource(std::move(Resource));
    }

    template <typename T>
    [[nodiscard]] auto PushConstants(Uint32 Offset, const T& Data) -> std::expected<void, ErrorMessage> {
        auto* BindingSet = GetShaderBindingSet(m_Pipeline);
        if (!BindingSet)
            return std::unexpected(ErrorMessage("Pass has no pipeline shader binding set"));
        return BindingSet->PushConstants(Offset, Data);
    }

    [[nodiscard]] auto Commit() -> std::expected<void, ErrorMessage> {
        auto* BindingSet = GetShaderBindingSet(m_Pipeline);
        if (!BindingSet)
            return std::unexpected(ErrorMessage("Pass has no pipeline shader binding set"));
        return BindingSet->Commit();
    }

    RHIPipeline             m_Pipeline = {};
    std::vector<RHICommand> m_Commands = {};
};

class IRHIGraphicsPass : public IRHIPass {
  public:
    explicit IRHIGraphicsPass(RHIRef<RHIGraphicsPipeline> Pipeline)
        : IRHIPass(RHIPipeline{std::move(Pipeline)}) {}

    [[nodiscard]] auto GetType() const noexcept -> RHIPassType override {
        return RHIPassType::Graphics;
    }

    [[nodiscard]] auto GetAttachments() const noexcept -> const RHIGraphicsAttachments& {
        return m_Attachments;
    }

    auto SetPresentOutput() -> void {
        m_PresentOutput = true;
    }

    [[nodiscard]] auto HasPresentOutput() const noexcept -> bool {
        return m_PresentOutput;
    }

  protected:
    auto SetViewport(Float32 X,
                     Float32 Y,
                     Float32 W,
                     Float32 H,
                     Float32 MinDepth = 0.0f,
                     Float32 MaxDepth = 1.0f) -> void {
        m_Commands.emplace_back(RHISetViewportCmd{X, Y, W, H, MinDepth, MaxDepth});
    }

    auto SetScissorRect(Int32 X, Int32 Y, Uint32 W, Uint32 H) -> void {
        m_Commands.emplace_back(RHISetScissorCmd{X, Y, W, H});
    }

    auto Draw() -> void {
        m_Commands.emplace_back(RHIDrawCmd{});
    }

    auto DrawIndirect(RHIRef<RHITransientShaderStorageBuffer> IndirectBuffer,
                      Uint64                          Offset    = 0,
                      Uint32                          DrawCount = 1,
                      Uint32                          Stride    = sizeof(Uint32) * 4) -> void {
        if (IndirectBuffer.GetState() == RHIRefState::Unknown || DrawCount == 0 ||
            Stride < sizeof(Uint32) * 4)
            return;
        m_Commands.emplace_back(RHIDrawIndirectCmd{
            .IndirectBuffer = std::move(IndirectBuffer),
            .Offset         = Offset,
            .DrawCount      = DrawCount,
            .Stride         = Stride,
        });
    }

    auto DrawIndexed(std::array<RHIRef<RHIVertexBuffer>, kMaxVertexBufferBindings> VertexBufferRefs,
                     RHIRef<RHIIndexBuffer>                                        IndexBufferRef) -> void {
        if (!VertexBufferRefs[0] || !IndexBufferRef)
            return;
        for (Uint32 Index = 1; Index < VertexBufferRefs.size(); ++Index) {
            if (VertexBufferRefs[Index].GetState() != RHIRefState::Unknown && !VertexBufferRefs[Index])
                return;
        }
        m_Commands.emplace_back(RHIDrawIndexedCmd{
            .VertexBufferRefs = std::move(VertexBufferRefs),
            .IndexBufferRef   = std::move(IndexBufferRef),
        });
    }

    RHIGraphicsAttachments m_Attachments = {};
    bool                   m_PresentOutput = false;
};

class IRHIRayTracingPass : public IRHIPass {
  public:
    explicit IRHIRayTracingPass(RHIRef<RHIRayTracingPipeline> Pipeline)
        : IRHIPass(RHIPipeline{std::move(Pipeline)}) {}

    [[nodiscard]] auto GetType() const noexcept -> RHIPassType override {
        return RHIPassType::RayTracing;
    }

  protected:
    auto BuildOrUpdateTopLevelAccelerationStructure(
        RHIRef<RHITopLevelAccelerationStructure>          TargetRef,
        std::span<const RHIAccelerationStructureInstance> Instances,
        RHITopLevelAccelerationStructureBuildMode Mode = RHITopLevelAccelerationStructureBuildMode::Auto) -> void {
        if (!TargetRef)
            return;
        m_Commands.emplace_back(RHIBuildOrUpdateTopLevelAccelerationStructureCmd{
            .TargetRef = std::move(TargetRef),
            .Instances = std::vector<RHIAccelerationStructureInstance>{Instances.begin(), Instances.end()},
            .Mode      = Mode,
        });
    }

    auto TraceRays(Uint32 Width, Uint32 Height, Uint32 Depth = 1) -> void {
        m_Commands.emplace_back(RHITraceRaysCmd{
            .Width = Width,
            .Height = Height,
            .Depth = Depth,
        });
    }
};

/// @brief Complete frame packet transferred from RenderLoop to the RHI thread.
struct RenderPassList {
    RenderPassList()                               = default;
    RenderPassList(const RenderPassList&)          = delete;
    auto operator=(const RenderPassList&) -> RenderPassList& = delete;
    RenderPassList(RenderPassList&&) noexcept      = default;
    auto operator=(RenderPassList&&) noexcept -> RenderPassList& = default;

    std::vector<UPtr<IRHIPass>> Passes = {};
    std::optional<RHIImGuiPresentationOverlayCmd> ImGuiPresentationOverlay = std::nullopt;
};

} // namespace SoulEngine
