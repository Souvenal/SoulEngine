module;

export module RHI:Pass;

import Core;
import :Command;
import :Pipeline;

export import std;

export namespace SoulEngine {

enum class RHIPassType : Uint8 {
    Unknown = 0,
    Graphics,
    Compute,
    Transfer,
    RayTracing,
};

/// @brief Renderer-independent pass builder and final RHI execution packet.
class IRHIPass {
  public:
    explicit IRHIPass(String Name) : m_Name(std::move(Name)) {}
    IRHIPass(const IRHIPass&)                              = delete;
    auto operator=(const IRHIPass&) -> IRHIPass&           = delete;
    IRHIPass(IRHIPass&&)                                   = default;
    auto operator=(IRHIPass&&) -> IRHIPass&                = default;
    virtual ~IRHIPass()                                    = default;

    [[nodiscard]] virtual auto Record() -> std::expected<void, ErrorMessage> = 0;
    [[nodiscard]] virtual auto GetType() const noexcept -> RHIPassType       = 0;

    [[nodiscard]] auto GetName() const noexcept -> StringView {
        return m_Name;
    }

    /// Return the pipeline as a base pointer. nullptr for transfer passes.
    [[nodiscard]] virtual auto GetPipeline() const noexcept -> RHIPipeline* = 0;

    [[nodiscard]] auto GetCommands() const noexcept -> std::span<const RHICommand> {
        return m_Commands;
    }

  protected:
    /// Binds the sampled-image array used by the pass pipeline's bindless space.
    [[nodiscard]] auto BindBindlessResource(RHIRefArray<RHISampledTexture> Resource)
        -> std::expected<void, ErrorMessage> {
        auto* BindingSet = GetShaderBindingSet();
        if (!BindingSet)
            return std::unexpected(ErrorMessage("Pass has no pipeline shader binding set"));
        return BindingSet->BindBindlessResource(std::move(Resource));
    }

    [[nodiscard]] auto BindResources(std::span<const RHIShaderBindingRequest> Resources)
        -> std::expected<void, ErrorMessage> {
        auto* BindingSet = GetShaderBindingSet();
        if (!BindingSet)
            return std::unexpected(ErrorMessage("Pass has no pipeline shader binding set"));
        return BindingSet->BindResources(Resources);
    }

    template <typename T>
    [[nodiscard]] auto PushConstants(Uint32 Offset, const T& Data) -> std::expected<void, ErrorMessage> {
        auto* BindingSet = GetShaderBindingSet();
        if (!BindingSet)
            return std::unexpected(ErrorMessage("Pass has no pipeline shader binding set"));
        return BindingSet->PushConstants(Offset, Data);
    }

    [[nodiscard]] auto GetShaderBindingSet() const -> RHIShaderBindingSet* {
        auto* Pipeline = GetPipeline();
        if (!Pipeline)
            return nullptr;
        return Pipeline->GetShaderBindingSet().TryGet();
    }

    String                 m_Name     = {};
    std::vector<RHICommand> m_Commands = {};
};

class IRHIGraphicsPass : public IRHIPass {
  public:
    explicit IRHIGraphicsPass(String Name, RHIRef<RHIGraphicsPipeline> Pipeline)
        : IRHIPass(std::move(Name)), m_Pipeline(std::move(Pipeline)) {}

    [[nodiscard]] auto GetType() const noexcept -> RHIPassType override {
        return RHIPassType::Graphics;
    }

    [[nodiscard]] auto GetPipeline() const noexcept -> RHIPipeline* override {
        return m_Pipeline.TryGet();
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

  private:
    RHIRef<RHIGraphicsPipeline> m_Pipeline = nullptr;
};

class IRHIComputePass : public IRHIPass {
  public:
    /// A compute pass may carry an empty pipeline until dispatch commands are
    /// added with the compute pipeline stage.
    explicit IRHIComputePass(String Name, RHIRef<RHIComputePipeline> Pipeline = nullptr)
        : IRHIPass(std::move(Name)), m_Pipeline(std::move(Pipeline)) {}

    [[nodiscard]] auto GetType() const noexcept -> RHIPassType override {
        return RHIPassType::Compute;
    }

    [[nodiscard]] auto GetPipeline() const noexcept -> RHIPipeline* override {
        return m_Pipeline.TryGet();
    }

  protected:
    auto Dispatch(Uint32 GroupCountX, Uint32 GroupCountY = 1, Uint32 GroupCountZ = 1) -> void {
        if (GroupCountX == 0)
            return;
        m_Commands.emplace_back(RHIDispatchCmd{
            .GroupCountX = GroupCountX,
            .GroupCountY = GroupCountY,
            .GroupCountZ = GroupCountZ,
        });
    }

  private:
    RHIRef<RHIComputePipeline> m_Pipeline = nullptr;
};

class IRHITransferPass : public IRHIPass {
  public:
    explicit IRHITransferPass(String Name) : IRHIPass(std::move(Name)) {}

    [[nodiscard]] auto GetType() const noexcept -> RHIPassType override {
        return RHIPassType::Transfer;
    }

    [[nodiscard]] auto GetPipeline() const noexcept -> RHIPipeline* override {
        return nullptr;
    }

  protected:
    auto CopyTextureToBuffer(RHIRef<RHIRenderTarget>   Source,
                             Uint32                    SrcX,
                             Uint32                    SrcY,
                             RHIRef<RHIReadbackBuffer> Target,
                             Uint32                    SrcWidth  = 1,
                             Uint32                    SrcHeight = 1) -> void {
        if (!Source || !Target || SrcWidth == 0 || SrcHeight == 0)
            return;
        m_Commands.emplace_back(RHICopyTextureToBufferCmd{
            .Source    = std::move(Source),
            .SrcX      = SrcX,
            .SrcY      = SrcY,
            .SrcWidth  = SrcWidth,
            .SrcHeight = SrcHeight,
            .Target    = std::move(Target),
        });
    }
};

class IRHIRayTracingPass : public IRHIPass {
  public:
    explicit IRHIRayTracingPass(String Name, RHIRef<RHIRayTracingPipeline> Pipeline)
        : IRHIPass(std::move(Name)), m_Pipeline(std::move(Pipeline)) {}

    [[nodiscard]] auto GetType() const noexcept -> RHIPassType override {
        return RHIPassType::RayTracing;
    }

    [[nodiscard]] auto GetPipeline() const noexcept -> RHIPipeline* override {
        return m_Pipeline.TryGet();
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

  private:
    RHIRef<RHIRayTracingPipeline> m_Pipeline = nullptr;
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
