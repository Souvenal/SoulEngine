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

auto IRHIGraphicsPass::DrawIndirect(RHIBufferView IndirectBuffer, Uint32 DrawCount) -> void {
    m_Commands.emplace_back(RHIDrawIndirectCmd{.IndirectBuffer = std::move(IndirectBuffer), .DrawCount = DrawCount});
}

auto IRHIGraphicsPass::DrawIndexedIndirect(RHIBufferView                  IndirectBuffer,
                                           RHIBufferView                  IndexBuffer,
                                           std::span<const RHIBufferView> VertexBuffers,
                                           Uint32                         DrawCount) -> void {
    m_Commands.emplace_back(RHIDrawIndexedIndirectCmd{
        .VertexBuffers  = std::vector<RHIBufferView>(VertexBuffers.begin(), VertexBuffers.end()),
        .IndexBuffer    = std::move(IndexBuffer),
        .IndirectBuffer = std::move(IndirectBuffer),
        .DrawCount      = DrawCount});
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
    RHITextureView Source, Uint32 DstX, Uint32 DstY, Uint32 DstWidth, Uint32 DstHeight) -> void {
    m_Commands.emplace_back(RHIBlitToSwapchainCmd{
        .Source = std::move(Source), .DstX = DstX, .DstY = DstY, .DstWidth = DstWidth, .DstHeight = DstHeight});
}

auto IRHITransferPass::ReadbackTexture(
    RHITextureView Source, Uint32 SrcX, Uint32 SrcY, RHIRef<RHIReadback> Target, Uint32 SrcWidth, Uint32 SrcHeight)
    -> void {
    m_Commands.emplace_back(RHIReadbackTextureCmd{.Source    = std::move(Source),
                                                  .SrcX      = SrcX,
                                                  .SrcY      = SrcY,
                                                  .SrcWidth  = SrcWidth,
                                                  .SrcHeight = SrcHeight,
                                                  .Target    = std::move(Target)});
}

auto IRHIRayTracingPass::TraceRays(Uint32 Width, Uint32 Height, Uint32 Depth) -> void {
    m_Commands.emplace_back(RHITraceRaysCmd{
        .Width  = Width,
        .Height = Height,
        .Depth  = Depth,
    });
}

} // namespace SoulEngine
