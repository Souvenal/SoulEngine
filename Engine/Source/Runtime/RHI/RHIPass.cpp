module;

#include <magic_enum/magic_enum_flags.hpp>

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

auto IRHIGraphicsPass::DrawIndirect(RHIBufferView IndirectBuffer, Uint32 DrawCount)
    -> std::expected<void, ErrorMessage> {
    const auto* Buffer = IndirectBuffer.Buffer.TryGet();
    if (!Buffer)
        return std::unexpected(ErrorMessage("Indirect draw requires a ready buffer"));
    if (!magic_enum::enum_flags_test(Buffer->GetUsage(), RHIBufferUsage::Indirect) ||
        IndirectBuffer.Usage != RHIBufferUsage::Indirect || IndirectBuffer.Access != RHIAccess::Read)
        return std::unexpected(
            ErrorMessage("Indirect draw requires a readable indirect buffer view backed by indirect usage"));
    if (DrawCount == 0)
        return std::unexpected(ErrorMessage("Indirect draw count must be greater than zero"));
    if (IndirectBuffer.Offset % alignof(RHIIndirectCommand) != 0)
        return std::unexpected(ErrorMessage("Indirect draw view offset is misaligned"));
    if (IndirectBuffer.Offset > Buffer->GetSize())
        return std::unexpected(ErrorMessage("Indirect draw view offset exceeds its buffer"));
    const Uint64 Range =
        IndirectBuffer.Range == kRHIWholeBuffer ? Buffer->GetSize() - IndirectBuffer.Offset : IndirectBuffer.Range;
    if (Range > Buffer->GetSize() - IndirectBuffer.Offset || DrawCount > Range / sizeof(RHIIndirectCommand))
        return std::unexpected(ErrorMessage("Indirect draw range exceeds its buffer view"));
    m_Commands.emplace_back(RHIDrawIndirectCmd{.IndirectBuffer = std::move(IndirectBuffer), .DrawCount = DrawCount});
    return {};
}

auto IRHIGraphicsPass::DrawIndexedIndirect(RHIBufferView                  IndirectBuffer,
                                           RHIBufferView                  IndexBuffer,
                                           std::span<const RHIBufferView> VertexBuffers,
                                           Uint32 DrawCount) -> std::expected<void, ErrorMessage> {
    if (VertexBuffers.size() > kMaxVertexBufferBindings)
        return std::unexpected(ErrorMessage("Indexed draw exceeds the vertex buffer binding limit"));
    const auto* IndirectBufferPtr = IndirectBuffer.Buffer.TryGet();
    const auto* IndexBufferPtr    = IndexBuffer.Buffer.TryGet();
    if (!IndirectBufferPtr || !IndexBufferPtr)
        return std::unexpected(ErrorMessage("Indexed draw references a buffer that is not ready"));
    if (!magic_enum::enum_flags_test(IndirectBufferPtr->GetUsage(), RHIBufferUsage::Indirect) ||
        IndirectBuffer.Usage != RHIBufferUsage::Indirect || IndirectBuffer.Access != RHIAccess::Read)
        return std::unexpected(
            ErrorMessage("Indexed draw requires a readable indirect buffer view backed by indirect usage"));
    if (!magic_enum::enum_flags_test(IndexBufferPtr->GetUsage(), RHIBufferUsage::Index) ||
        IndexBuffer.Usage != RHIBufferUsage::Index || IndexBuffer.Access != RHIAccess::Read)
        return std::unexpected(
            ErrorMessage("Indexed draw requires a readable index buffer view backed by index usage"));
    if (DrawCount == 0)
        return std::unexpected(ErrorMessage("Indexed indirect draw count must be greater than zero"));
    if (IndirectBuffer.Offset % alignof(RHIIndexedIndirectCommand) != 0 || IndexBuffer.Offset % sizeof(Uint32) != 0)
        return std::unexpected(ErrorMessage("Indexed draw buffer view offset is misaligned"));
    if (IndirectBuffer.Offset > IndirectBufferPtr->GetSize() || IndexBuffer.Offset > IndexBufferPtr->GetSize())
        return std::unexpected(ErrorMessage("Indexed draw view offset exceeds its buffer"));
    const Uint64 IndirectRange = IndirectBuffer.Range == kRHIWholeBuffer
                                     ? IndirectBufferPtr->GetSize() - IndirectBuffer.Offset
                                     : IndirectBuffer.Range;
    if (IndirectRange > IndirectBufferPtr->GetSize() - IndirectBuffer.Offset ||
        DrawCount > IndirectRange / sizeof(RHIIndexedIndirectCommand))
        return std::unexpected(ErrorMessage("Indexed indirect draw range exceeds its buffer view"));
    const Uint64 IndexRange =
        IndexBuffer.Range == kRHIWholeBuffer ? IndexBufferPtr->GetSize() - IndexBuffer.Offset : IndexBuffer.Range;
    if (IndexRange > IndexBufferPtr->GetSize() - IndexBuffer.Offset || IndexRange < sizeof(Uint32))
        return std::unexpected(ErrorMessage("Indexed draw index range is empty or exceeds its buffer view"));
    for (const auto& VertexView : VertexBuffers) {
        const auto* VertexBuffer = VertexView.Buffer.TryGet();
        if (!VertexBuffer || !magic_enum::enum_flags_test(VertexBuffer->GetUsage(), RHIBufferUsage::Vertex) ||
            VertexView.Usage != RHIBufferUsage::Vertex || VertexView.Access != RHIAccess::Read)
            return std::unexpected(ErrorMessage("Indexed draw requires readable vertex views backed by vertex usage"));
        if (VertexView.Offset % sizeof(Uint32) != 0 || VertexView.Offset > VertexBuffer->GetSize())
            return std::unexpected(ErrorMessage("Indexed draw vertex buffer view offset is invalid"));
        const Uint64 VertexRange =
            VertexView.Range == kRHIWholeBuffer ? VertexBuffer->GetSize() - VertexView.Offset : VertexView.Range;
        if (VertexRange == 0 || VertexRange > VertexBuffer->GetSize() - VertexView.Offset ||
            VertexRange % sizeof(Uint32) != 0)
            return std::unexpected(ErrorMessage("Indexed draw vertex buffer view range is invalid"));
    }
    RHIDrawIndexedIndirectCmd Cmd{.VertexBuffers =
                                      std::vector<RHIBufferView>(VertexBuffers.begin(), VertexBuffers.end()),
                                  .IndexBuffer    = std::move(IndexBuffer),
                                  .IndirectBuffer = std::move(IndirectBuffer),
                                  .DrawCount      = DrawCount};
    m_Commands.emplace_back(std::move(Cmd));
    return {};
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

auto IRHITransferPass::BlitToSwapchain(RHITextureView Source,
                                       Uint32         DstX,
                                       Uint32         DstY,
                                       Uint32         DstWidth,
                                       Uint32         DstHeight) -> std::expected<void, ErrorMessage> {
    const auto* Texture = Source.Texture.TryGet();
    if (!Texture)
        return std::unexpected(ErrorMessage("Blit-to-swapchain source is not ready"));
    if (!magic_enum::enum_flags_test(Texture->GetUsage(), RHITextureUsage::TransferSrc) ||
        Source.Usage != RHITextureUsage::TransferSrc || Source.Access != RHIAccess::Read)
        return std::unexpected(ErrorMessage("Blit-to-swapchain requires a readable transfer-source view"));
    if (DstWidth == 0 || DstHeight == 0)
        return std::unexpected(ErrorMessage("Blit-to-swapchain destination extent must be nonzero"));
    if (Source.Range.BaseMipLevel >= 32 || Source.Range.BaseMipLevel >= Texture->GetMipLevelCount() ||
        (Source.Range.MipLevelCount != 1 && (Source.Range.MipLevelCount != kRHIRemainingSubresources ||
                                             Source.Range.BaseMipLevel + 1 != Texture->GetMipLevelCount())))
        return std::unexpected(ErrorMessage("Blit-to-swapchain view must select one valid mip"));
    m_Commands.emplace_back(RHIBlitToSwapchainCmd{
        .Source = std::move(Source), .DstX = DstX, .DstY = DstY, .DstWidth = DstWidth, .DstHeight = DstHeight});
    return {};
}

auto IRHITransferPass::ReadbackTexture(
    RHITextureView Source, Uint32 SrcX, Uint32 SrcY, RHIRef<RHIReadback> Target, Uint32 SrcWidth, Uint32 SrcHeight)
    -> std::expected<void, ErrorMessage> {
    const auto* Texture = Source.Texture.TryGet();
    if (!Texture || !Target)
        return std::unexpected(ErrorMessage("Texture readback requires ready source and target resources"));
    if (!magic_enum::enum_flags_test(Texture->GetUsage(), RHITextureUsage::TransferSrc) ||
        Source.Usage != RHITextureUsage::TransferSrc || Source.Access != RHIAccess::Read)
        return std::unexpected(ErrorMessage("Texture readback requires a readable transfer-source view"));
    const auto Format = Texture->GetFormat();
    if (Format != RHIFormat::R8G8B8A8_UNORM && Format != RHIFormat::B8G8R8A8_UNORM && Format != RHIFormat::R32_SFLOAT &&
        Format != RHIFormat::R32_UINT)
        return std::unexpected(ErrorMessage("Texture readback requires a 32-bit color texel format"));
    if (SrcWidth == 0 || SrcHeight == 0)
        return std::unexpected(ErrorMessage("Texture readback extent must be nonzero"));
    if (Source.Range.BaseMipLevel >= 32 || Source.Range.BaseMipLevel >= Texture->GetMipLevelCount() ||
        (Source.Range.MipLevelCount != 1 && (Source.Range.MipLevelCount != kRHIRemainingSubresources ||
                                             Source.Range.BaseMipLevel + 1 != Texture->GetMipLevelCount())))
        return std::unexpected(ErrorMessage("Texture readback view must select one valid mip"));
    const Uint32 MipWidth  = (std::max)(Texture->GetWidth() >> Source.Range.BaseMipLevel, 1u);
    const Uint32 MipHeight = (std::max)(Texture->GetHeight() >> Source.Range.BaseMipLevel, 1u);
    if (SrcX > MipWidth || SrcY > MipHeight || SrcWidth > MipWidth - SrcX || SrcHeight > MipHeight - SrcY)
        return std::unexpected(ErrorMessage("Texture readback region exceeds its selected mip"));
    m_Commands.emplace_back(RHIReadbackTextureCmd{.Source    = std::move(Source),
                                                  .SrcX      = SrcX,
                                                  .SrcY      = SrcY,
                                                  .SrcWidth  = SrcWidth,
                                                  .SrcHeight = SrcHeight,
                                                  .Target    = std::move(Target)});
    return {};
}

auto IRHIRayTracingPass::TraceRays(Uint32 Width, Uint32 Height, Uint32 Depth) -> void {
    m_Commands.emplace_back(RHITraceRaysCmd{
        .Width  = Width,
        .Height = Height,
        .Depth  = Depth,
    });
}

} // namespace SoulEngine
