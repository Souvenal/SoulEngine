module RHI.Pass;

import Core;

import RHI.Command;
import RHI.Pipeline;
import RHI.Types;
import RHI.Ref;

import std;

namespace SoulEngine {

auto IRHIPass::BindResources(std::span<const RHIShaderBindingRequest> Resources) -> std::expected<void, ErrorMessage> {
    auto* BindingSet = GetShaderBindingSet();
    if (!BindingSet)
        return std::unexpected(ErrorMessage("Pass has no pipeline shader binding set"));
    return BindingSet->BindResources(Resources);
}

auto IRHIPass::BindArrayResources(std::span<const RHIShaderArrayBindingRequest> Requests)
    -> std::expected<void, ErrorMessage> {
    auto* BindingSet = GetShaderBindingSet();
    if (!BindingSet)
        return std::unexpected(ErrorMessage("Pass has no pipeline shader binding set"));
    return BindingSet->BindArrayResources(Requests);
}

auto IRHIPass::GetShaderBindingSet() const -> RHIShaderBindingSet* {
    auto* Pipeline = GetPipeline();
    if (!Pipeline)
        return nullptr;
    return Pipeline->GetShaderBindingSet().TryGet();
}

auto IRHIGraphicsPass::SetViewport(Float32 X, Float32 Y, Float32 W, Float32 H, Float32 MinDepth, Float32 MaxDepth)
    -> void {
    m_Commands.emplace_back(RHISetViewportCmd{X, Y, W, H, MinDepth, MaxDepth});
}

auto IRHIGraphicsPass::SetScissorRect(Int32 X, Int32 Y, Uint32 W, Uint32 H) -> void {
    m_Commands.emplace_back(RHISetScissorCmd{X, Y, W, H});
}

auto IRHIGraphicsPass::Draw() -> void {
    m_Commands.emplace_back(RHIDrawCmd{});
}

auto IRHIGraphicsPass::DrawIndirect(RHIRef<RHIBuffer> IndirectBuffer, Uint64 Offset, Uint32 DrawCount, Uint32 Stride)
    -> void {
    if (IndirectBuffer.GetState() == RHIRefState::Unknown || DrawCount == 0 || Stride < sizeof(Uint32) * 4)
        return;
    m_Commands.emplace_back(RHIDrawIndirectCmd{
        .IndirectBuffer = std::move(IndirectBuffer),
        .Offset         = Offset,
        .DrawCount      = DrawCount,
        .Stride         = Stride,
    });
}

auto IRHIGraphicsPass::BindVertexBuffers(std::span<const RHIVertexBufferBinding> Bindings) -> void {
    if (Bindings.empty() || Bindings.size() > kMaxVertexBufferBindings) {
        return;
    }
    RHIBindVertexBuffersCmd Cmd{.BindingCount = static_cast<Uint32>(Bindings.size())};
    std::ranges::copy(Bindings, Cmd.Bindings.begin());
    if (!Cmd.Validate()) {
        return;
    }
    m_Commands.emplace_back(std::move(Cmd));
}

auto IRHIGraphicsPass::BindIndexBuffer(RHIRef<RHIBuffer> IndexBuffer, Uint64 Offset) -> void {
    RHIBindIndexBufferCmd Cmd{.IndexBuffer = std::move(IndexBuffer), .Offset = Offset};
    if (!Cmd.Validate())
        return;
    m_Commands.emplace_back(std::move(Cmd));
}

auto IRHIGraphicsPass::DrawIndexedIndirect(RHIRef<RHIBuffer> IndirectBuffer,
                                           Uint64            Offset,
                                           Uint32            DrawCount,
                                           Uint32            Stride) -> void {
    RHIDrawIndexedIndirectCmd Cmd{
        .IndirectBuffer = std::move(IndirectBuffer),
        .Offset         = Offset,
        .DrawCount      = DrawCount,
        .Stride         = Stride,
    };
    const auto* Buffer = Cmd.IndirectBuffer.TryGet();
    if (!Buffer || !Cmd.Validate(Buffer->GetSize(), std::numeric_limits<Uint32>::max()))
        return;
    m_Commands.emplace_back(std::move(Cmd));
}

auto IRHIComputePass::Dispatch(Uint32 GroupCountX, Uint32 GroupCountY, Uint32 GroupCountZ) -> void {
    if (GroupCountX == 0)
        return;
    m_Commands.emplace_back(RHIDispatchCmd{
        .GroupCountX = GroupCountX,
        .GroupCountY = GroupCountY,
        .GroupCountZ = GroupCountZ,
    });
}

auto IRHITransferPass::BlitToSwapchain(
    RHIRef<RHITexture> Source, Uint32 DstX, Uint32 DstY, Uint32 DstWidth, Uint32 DstHeight) -> void {
    if (!Source || DstWidth == 0 || DstHeight == 0)
        return;
    m_Commands.emplace_back(RHIBlitToSwapchainCmd{
        .Source    = std::move(Source),
        .DstX      = DstX,
        .DstY      = DstY,
        .DstWidth  = DstWidth,
        .DstHeight = DstHeight,
    });
}

auto IRHITransferPass::ReadbackTexture(
    RHIRef<RHITexture> Source, Uint32 SrcX, Uint32 SrcY, RHIRef<RHIReadback> Target, Uint32 SrcWidth, Uint32 SrcHeight)
    -> void {
    if (!Source || !Target || SrcWidth == 0 || SrcHeight == 0)
        return;
    m_Commands.emplace_back(RHIReadbackTextureCmd{
        .Source    = std::move(Source),
        .SrcX      = SrcX,
        .SrcY      = SrcY,
        .SrcWidth  = SrcWidth,
        .SrcHeight = SrcHeight,
        .Target    = std::move(Target),
    });
}

auto IRHIRayTracingPass::TraceRays(Uint32 Width, Uint32 Height, Uint32 Depth) -> void {
    m_Commands.emplace_back(RHITraceRaysCmd{
        .Width  = Width,
        .Height = Height,
        .Depth  = Depth,
    });
}

} // namespace SoulEngine
