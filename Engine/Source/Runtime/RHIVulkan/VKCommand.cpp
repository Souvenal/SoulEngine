module;

#include <magic_enum/magic_enum_flags.hpp>

module RHIVulkan.Command;

import Core;
import RHI;
import vulkan;
import std;

import RHIVulkan.Types;
import RHIVulkan.Context;
import RHIVulkan.Capability;
import RHIVulkan.Swapchain;
import RHIVulkan.Buffer;
import RHIVulkan.Readback;
import RHIVulkan.Pipeline;
import RHIVulkan.RayTracingPipeline;
import RHIVulkan.Sampler;
import RHIVulkan.Texture;
import RHIVulkan.Descriptor;
import RHIVulkan.AccelerationStructure;
import RHIVulkan.ShaderBindingSet;
import Shader;

namespace SoulEngine {

[[nodiscard]] auto ToVkClearColor(const RHIClearColorValue& Value) -> vk::ClearColorValue {
    return std::visit(
        []<typename T>(const T& ClearValue) -> vk::ClearColorValue {
            vk::ClearColorValue Result = {};
            if constexpr (std::is_same_v<T, std::array<float, 4>>)
                Result.float32 = ClearValue;
            else if constexpr (std::is_same_v<T, std::array<std::int32_t, 4>>)
                Result.int32 = ClearValue;
            else if constexpr (std::is_same_v<T, std::array<std::uint32_t, 4>>)
                Result.uint32 = ClearValue;
            return Result;
        },
        Value);
}

auto VulkanCmdVisitor::BindShaderBindingSet(VulkanShaderBindingSet& BindingSet, vk::PipelineBindPoint BindPoint)
    -> void {
    const vk::PipelineLayout Layout = BindingSet.GetPipelineLayout();

    auto State = BindingSet.CaptureBindingState();
    if (!State) {
        Error = State.error().Append("Failed to capture shader binding state");
        return;
    }
    if (!State->DescriptorSets.empty())
        Buf.bindDescriptorSets(BindPoint, Layout, 0, State->DescriptorSets, State->DynamicOffsets);
    for (const auto& PushConstant : State->PushConstants) {
        Buf.pushConstants(Layout,
                          BindingSet.GetShaderStageFlags(),
                          PushConstant.Offset,
                          static_cast<Uint32>(PushConstant.Data.size()),
                          PushConstant.Data.data());
    }
}

VulkanGraphicsCmdVisitor::~VulkanGraphicsCmdVisitor() {
    if (m_IsRendering)
        Buf.endRendering();
}

VulkanGraphicsCmdVisitor::VulkanGraphicsCmdVisitor(VulkanGraphicsPipeline&       Pipeline,
                                                   const RHIGraphicsAttachments& Attachments,
                                                   vk::raii::CommandBuffer&      InBuffer,
                                                   VulkanResourceContext&        InResourceContext)
    : VulkanCmdVisitor(InBuffer, InResourceContext), m_Pipeline(Pipeline) {
    Buf.bindPipeline(vk::PipelineBindPoint::eGraphics, Pipeline.Get());
    auto* BindingSet = Pipeline.GetShaderBindingSet().TryGet();
    if (BindingSet)
        BindShaderBindingSet(static_cast<VulkanShaderBindingSet&>(*BindingSet), vk::PipelineBindPoint::eGraphics);
    BeginRendering(Attachments);
}

auto VulkanGraphicsCmdVisitor::BeginRendering(const RHIGraphicsAttachments& Attachments) -> void {
    if (Error || m_IsRendering)
        return;

    std::vector<vk::RenderingAttachmentInfo> ColorAttachments = {};
    ColorAttachments.reserve(Attachments.ColorAttachments.size());
    m_CurrentColorFormats.clear();
    m_CurrentColorFormats.reserve(Attachments.ColorAttachments.size());
    m_CurrentDepthFormat = RHIFormat::Unknown;
    Uint32 RenderWidth   = 0;
    Uint32 RenderHeight  = 0;

    for (const auto& AttachmentDesc : Attachments.ColorAttachments) {
        auto* ColorTarget = AttachmentDesc.View.GetTexture().TryGet();
        if (!ColorTarget) {
            Error = ErrorMessage("Rendering pass color attachment is not ready");
            return;
        }
        const auto& ColorRT = static_cast<const VulkanTexture&>(*ColorTarget);
        if (AttachmentDesc.Clear && std::holds_alternative<std::monostate>(AttachmentDesc.ClearValue)) {
            Error = ErrorMessage("Rendering pass color attachment clear value is not set");
            return;
        }
        ColorAttachments.push_back(vk::RenderingAttachmentInfo{
            .imageView   = AttachmentDesc.View.GetRange().MipLevelCount == kRHIRemainingSubresources
                               ? ColorRT.GetVkImageView()
                               : ColorRT.GetVkImageView(AttachmentDesc.View.GetRange().BaseMipLevel),
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp      = AttachmentDesc.Clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp     = vk::AttachmentStoreOp::eStore,
            .clearValue  = vk::ClearValue{.color = ToVkClearColor(AttachmentDesc.ClearValue)},
        });
        m_CurrentColorFormats.push_back(ColorRT.GetFormat());
        RenderWidth  = RenderWidth == 0 ? ColorRT.GetWidth() : (std::min)(RenderWidth, ColorRT.GetWidth());
        RenderHeight = RenderHeight == 0 ? ColorRT.GetHeight() : (std::min)(RenderHeight, ColorRT.GetHeight());
    }
    m_CurrentRenderExtent = vk::Extent2D{RenderWidth, RenderHeight};

    std::optional<vk::RenderingAttachmentInfo> DepthAttachment = std::nullopt;
    if (Attachments.DepthAttachment.has_value()) {
        auto* DepthTarget = Attachments.DepthAttachment->View.GetTexture().TryGet();
        if (!DepthTarget) {
            Error = ErrorMessage("Rendering pass depth attachment is not ready");
            return;
        }
        auto& VkDepthRT      = static_cast<const VulkanTexture&>(*DepthTarget);
        m_CurrentDepthFormat = VkDepthRT.GetFormat();
        DepthAttachment      = vk::RenderingAttachmentInfo{
            .imageView   = Attachments.DepthAttachment->View.GetRange().MipLevelCount == kRHIRemainingSubresources
                               ? VkDepthRT.GetVkImageView()
                               : VkDepthRT.GetVkImageView(Attachments.DepthAttachment->View.GetRange().BaseMipLevel),
            .imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
            .loadOp  = Attachments.DepthAttachment->Clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue =
                vk::ClearValue{.depthStencil =
                                   vk::ClearDepthStencilValue{Attachments.DepthAttachment->ClearValue.Depth,
                                                              Attachments.DepthAttachment->ClearValue.Stencil}},
        };
        // Clamp render area to smallest attachment dimension. Vulkan requires every attachment imageView
        // extent ≥ renderArea, so if the depth RT is smaller than the color RT (or vice versa) we must
        // shrink renderArea to fit the minimum.
        m_CurrentRenderExtent.width  = (std::min)(m_CurrentRenderExtent.width, VkDepthRT.GetWidth());
        m_CurrentRenderExtent.height = (std::min)(m_CurrentRenderExtent.height, VkDepthRT.GetHeight());
    }

    if (!m_CurrentColorFormats.empty() && !m_Pipeline.IsCompatibleWith(m_CurrentColorFormats, m_CurrentDepthFormat)) {
        Error = ErrorMessage("Graphics pipeline attachment formats do not match the active rendering pass");
        return;
    }

    Buf.beginRendering(vk::RenderingInfo{
        .renderArea           = vk::Rect2D{{0, 0}, m_CurrentRenderExtent},
        .layerCount           = 1,
        .colorAttachmentCount = static_cast<Uint32>(ColorAttachments.size()),
        .pColorAttachments    = ColorAttachments.data(),
        .pDepthAttachment     = DepthAttachment.has_value() ? &*DepthAttachment : nullptr,
    });
    m_IsRendering = true;
}

auto VulkanGraphicsCmdVisitor::operator()(const RHISetViewportCmd& Cmd) -> void {
    Buf.setViewport(0, {vk::Viewport{Cmd.X, Cmd.Y + Cmd.Height, Cmd.Width, -Cmd.Height, Cmd.MinDepth, Cmd.MaxDepth}});
}

auto VulkanGraphicsCmdVisitor::operator()(const RHISetScissorCmd& Cmd) -> void {
    Buf.setScissor(0, {vk::Rect2D{{Cmd.X, Cmd.Y}, {Cmd.Width, Cmd.Height}}});
}

auto VulkanGraphicsCmdVisitor::BindVertexBuffers(std::span<const RHIBufferView> VertexBuffers) -> void {
    const Uint32 MaxVertexInputBindings = VulkanCapability::Get().GetProperties().limits.maxVertexInputBindings;
    if (VertexBuffers.size() > MaxVertexInputBindings) {
        Error = ErrorMessage(Format(
            "Draw has {} vertex buffers, exceeding the device limit {}", VertexBuffers.size(), MaxVertexInputBindings));
        return;
    }
    for (const auto& Binding : m_Pipeline.GetVertexInputLayout().Bindings) {
        if (Binding.Binding >= VertexBuffers.size()) {
            Error = ErrorMessage("Draw is missing a pipeline vertex input binding");
            return;
        }
    }
    if (VertexBuffers.empty())
        return;

    std::vector<vk::Buffer>     Handles(VertexBuffers.size());
    std::vector<vk::DeviceSize> Offsets(VertexBuffers.size());
    for (std::size_t I = 0; I < VertexBuffers.size(); ++I) {
        const auto& View     = VertexBuffers[I];
        const auto* Resource = View.GetBuffer().TryGet();
        if (!Resource || View.GetUsage() != RHIBufferUsage::Vertex || View.GetAccess() != RHIAccess::Read ||
            View.GetOffset() % sizeof(Uint32) != 0 || View.GetOffset() >= Resource->GetSize()) {
            Error = ErrorMessage("Draw has an invalid vertex buffer view");
            return;
        }
        const Uint64 VertexRange =
            View.GetRange() == kRHIWholeBuffer ? Resource->GetSize() - View.GetOffset() : View.GetRange();
        if (VertexRange == 0 || VertexRange > Resource->GetSize() - View.GetOffset() ||
            VertexRange % sizeof(Uint32) != 0) {
            Error = ErrorMessage("Draw vertex buffer view range is invalid");
            return;
        }
        const auto& Vertex = static_cast<const VulkanBuffer&>(*Resource);
        const auto* Prop   = Vertex.GetProp<TransientBufferProp>();
        Handles[I]         = Vertex.GetVkBuffer();
        Offsets[I]         = (Prop ? Prop->Allocation.Offset : 0) + View.GetOffset();
    }
    Buf.bindVertexBuffers(0, Handles, Offsets);
}

auto VulkanGraphicsCmdVisitor::operator()(const RHIDrawCmd& Cmd) -> void {
    BindVertexBuffers(Cmd.VertexBuffers);
    if (Error)
        return;
    Buf.draw(Cmd.VertexCount, Cmd.InstanceCount, Cmd.FirstVertex, Cmd.FirstInstance);
}

auto VulkanGraphicsCmdVisitor::operator()(const RHIDrawIndexedCmd& Cmd) -> void {
    const auto* IndexBufferPtr = Cmd.IndexBuffer.GetBuffer().TryGet();
    if (!IndexBufferPtr || Cmd.IndexBuffer.GetUsage() != RHIBufferUsage::Index ||
        Cmd.IndexBuffer.GetAccess() != RHIAccess::Read) {
        Error = ErrorMessage("Indexed draw requires a ready readable index buffer view");
        return;
    }
    if (Cmd.IndexBuffer.GetOffset() % sizeof(Uint32) != 0 || Cmd.IndexBuffer.GetOffset() > IndexBufferPtr->GetSize()) {
        Error = ErrorMessage("Indexed draw index buffer view offset is invalid");
        return;
    }
    const Uint64 IndexRange = Cmd.IndexBuffer.GetRange() == kRHIWholeBuffer
                                  ? IndexBufferPtr->GetSize() - Cmd.IndexBuffer.GetOffset()
                                  : Cmd.IndexBuffer.GetRange();
    if (IndexRange > IndexBufferPtr->GetSize() - Cmd.IndexBuffer.GetOffset() || IndexRange < sizeof(Uint32)) {
        Error = ErrorMessage("Indexed draw index range is invalid");
        return;
    }
    const Uint64 AvailableIndices = IndexRange / sizeof(Uint32);
    if (Cmd.FirstIndex > AvailableIndices || Cmd.IndexCount > AvailableIndices - Cmd.FirstIndex) {
        Error = ErrorMessage("Indexed draw index count exceeds its buffer view");
        return;
    }

    BindVertexBuffers(Cmd.VertexBuffers);
    if (Error)
        return;
    const auto& Index = static_cast<const VulkanBuffer&>(*IndexBufferPtr);
    const auto* Prop  = Index.GetProp<TransientBufferProp>();
    Buf.bindIndexBuffer(Index.GetVkBuffer(),
                        (Prop ? Prop->Allocation.Offset : 0) + Cmd.IndexBuffer.GetOffset(),
                        vk::IndexType::eUint32);
    Buf.drawIndexed(Cmd.IndexCount, Cmd.InstanceCount, Cmd.FirstIndex, Cmd.VertexOffset, Cmd.FirstInstance);
}

auto VulkanGraphicsCmdVisitor::operator()(const RHIDrawIndirectCmd& Cmd) -> void {
    const auto* Buffer = Cmd.IndirectBuffer.GetBuffer().TryGet();
    if (!Buffer || Cmd.IndirectBuffer.GetUsage() != RHIBufferUsage::Indirect ||
        Cmd.IndirectBuffer.GetAccess() != RHIAccess::Read) {
        Error = ErrorMessage("Indirect draw requires a ready readable indirect buffer view");
        return;
    }
    if (Cmd.DrawCount == 0 || Cmd.DrawCount > VulkanCapability::Get().GetProperties().limits.maxDrawIndirectCount) {
        Error = ErrorMessage("Indirect draw count is outside the Vulkan supported range");
        return;
    }
    if (Cmd.IndirectBuffer.GetOffset() % alignof(RHIIndirectCommand) != 0 ||
        Cmd.IndirectBuffer.GetOffset() > Buffer->GetSize()) {
        Error = ErrorMessage("Indirect draw view offset exceeds its buffer");
        return;
    }
    const Uint64 ViewRange = Cmd.IndirectBuffer.GetRange() == kRHIWholeBuffer
                                 ? Buffer->GetSize() - Cmd.IndirectBuffer.GetOffset()
                                 : Cmd.IndirectBuffer.GetRange();
    if (ViewRange > Buffer->GetSize() - Cmd.IndirectBuffer.GetOffset() ||
        Cmd.DrawCount > ViewRange / sizeof(RHIIndirectCommand)) {
        Error = ErrorMessage("Indirect draw range exceeds its buffer view");
        return;
    }
    const auto& Indirect = static_cast<const VulkanBuffer&>(*Buffer);
    const auto* Prop     = Indirect.GetProp<TransientBufferProp>();
    if (!Prop) {
        Error = ErrorMessage("Indirect draw buffer has no transient state");
        return;
    }
    BindVertexBuffers(Cmd.VertexBuffers);
    if (Error)
        return;
    Buf.drawIndirect(Indirect.GetVkBuffer(),
                     Prop->Allocation.Offset + Cmd.IndirectBuffer.GetOffset(),
                     Cmd.DrawCount,
                     sizeof(RHIIndirectCommand));
}

auto VulkanGraphicsCmdVisitor::operator()(const RHIDrawIndexedIndirectCmd& Cmd) -> void {
    if (Cmd.DrawCount == 0 || Cmd.DrawCount > VulkanCapability::Get().GetProperties().limits.maxDrawIndirectCount) {
        Error = ErrorMessage("Indexed indirect draw has invalid vertex-buffer or draw-count values");
        return;
    }
    const auto* IndirectBufferPtr = Cmd.IndirectBuffer.GetBuffer().TryGet();
    const auto* IndexBufferPtr    = Cmd.IndexBuffer.GetBuffer().TryGet();
    if (!IndirectBufferPtr || !IndexBufferPtr || Cmd.IndirectBuffer.GetUsage() != RHIBufferUsage::Indirect ||
        Cmd.IndexBuffer.GetUsage() != RHIBufferUsage::Index || Cmd.IndirectBuffer.GetAccess() != RHIAccess::Read ||
        Cmd.IndexBuffer.GetAccess() != RHIAccess::Read) {
        Error = ErrorMessage("Indexed draw requires ready readable indirect and index buffer views");
        return;
    }
    if (Cmd.IndirectBuffer.GetOffset() % alignof(RHIIndexedIndirectCommand) != 0 ||
        Cmd.IndexBuffer.GetOffset() % sizeof(Uint32) != 0 ||
        Cmd.IndirectBuffer.GetOffset() > IndirectBufferPtr->GetSize() ||
        Cmd.IndexBuffer.GetOffset() > IndexBufferPtr->GetSize()) {
        Error = ErrorMessage("Indexed draw buffer view offset is invalid");
        return;
    }
    const Uint64 IndirectRange = Cmd.IndirectBuffer.GetRange() == kRHIWholeBuffer
                                     ? IndirectBufferPtr->GetSize() - Cmd.IndirectBuffer.GetOffset()
                                     : Cmd.IndirectBuffer.GetRange();
    if (IndirectRange > IndirectBufferPtr->GetSize() - Cmd.IndirectBuffer.GetOffset() ||
        Cmd.DrawCount > IndirectRange / sizeof(RHIIndexedIndirectCommand)) {
        Error = ErrorMessage("Indexed indirect draw range exceeds its buffer view");
        return;
    }
    const Uint64 IndexRange = Cmd.IndexBuffer.GetRange() == kRHIWholeBuffer
                                  ? IndexBufferPtr->GetSize() - Cmd.IndexBuffer.GetOffset()
                                  : Cmd.IndexBuffer.GetRange();
    if (IndexRange > IndexBufferPtr->GetSize() - Cmd.IndexBuffer.GetOffset() || IndexRange < sizeof(Uint32)) {
        Error = ErrorMessage("Indexed draw index range is invalid");
        return;
    }
    const auto& Indirect     = static_cast<const VulkanBuffer&>(*IndirectBufferPtr);
    const auto* IndirectProp = Indirect.GetProp<TransientBufferProp>();
    if (!IndirectProp) {
        Error = ErrorMessage("Indexed indirect draw buffer has no transient state");
        return;
    }
    const auto& Index     = static_cast<const VulkanBuffer&>(*IndexBufferPtr);
    const auto* IndexProp = Index.GetProp<TransientBufferProp>();
    BindVertexBuffers(Cmd.VertexBuffers);
    if (Error)
        return;
    Buf.bindIndexBuffer(Index.GetVkBuffer(),
                        (IndexProp ? IndexProp->Allocation.Offset : 0) + Cmd.IndexBuffer.GetOffset(),
                        vk::IndexType::eUint32);
    Buf.drawIndexedIndirect(Indirect.GetVkBuffer(),
                            IndirectProp->Allocation.Offset + Cmd.IndirectBuffer.GetOffset(),
                            Cmd.DrawCount,
                            sizeof(RHIIndexedIndirectCommand));
}

VulkanComputeCmdVisitor::VulkanComputeCmdVisitor(VulkanComputePipeline&   Pipeline,
                                                 vk::raii::CommandBuffer& InBuffer,
                                                 VulkanResourceContext&   InResourceContext)
    : VulkanCmdVisitor(InBuffer, InResourceContext), m_Pipeline(Pipeline) {
    Buf.bindPipeline(vk::PipelineBindPoint::eCompute, Pipeline.Get());
    auto* BindingSet = Pipeline.GetShaderBindingSet().TryGet();
    if (BindingSet)
        BindShaderBindingSet(static_cast<VulkanShaderBindingSet&>(*BindingSet), vk::PipelineBindPoint::eCompute);
}

auto VulkanComputeCmdVisitor::operator()(const RHIDispatchCmd& Cmd) -> void {
    Buf.dispatch(Cmd.GroupCountX, Cmd.GroupCountY, Cmd.GroupCountZ);
}

VulkanRayTracingCmdVisitor::VulkanRayTracingCmdVisitor(VulkanRayTracingPipeline& Pipeline,
                                                       vk::raii::CommandBuffer&  InBuffer,
                                                       VulkanResourceContext&    InResourceContext)
    : VulkanCmdVisitor(InBuffer, InResourceContext), m_Pipeline(Pipeline) {
    Buf.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, Pipeline.Get());
    auto* BindingSet = Pipeline.GetShaderBindingSet().TryGet();
    if (BindingSet)
        BindShaderBindingSet(static_cast<VulkanShaderBindingSet&>(*BindingSet), vk::PipelineBindPoint::eRayTracingKHR);
}

auto VulkanRayTracingCmdVisitor::operator()(const RHITraceRaysCmd& Cmd) -> void {
    Buf.traceRaysKHR(m_Pipeline.GetRayGenerationRegion(),
                     m_Pipeline.GetMissRegion(),
                     m_Pipeline.GetHitRegion(),
                     m_Pipeline.GetCallableRegion(),
                     Cmd.Width,
                     Cmd.Height,
                     Cmd.Depth);
    // Cross-frame hazard: the TLAS is a single structure, so the next frame's TLAS build must not start
    // while this frame's trace may still be reading it. Same-queue submissions may overlap without an
    // explicit barrier.
    const vk::MemoryBarrier2 TraceBarrier{
        .srcStageMask  = vk::PipelineStageFlagBits2::eRayTracingShaderKHR,
        .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR,
        .dstStageMask  = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
        .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
    };
    Buf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &TraceBarrier});
}

auto VulkanTransferCmdVisitor::operator()(const RHIBlitToSwapchainCmd& Cmd) -> void {
    const auto* SourcePtr = Cmd.Source.GetTexture().TryGet();
    if (!SourcePtr || Cmd.Source.GetUsage() != RHITextureUsage::TransferSrc ||
        Cmd.Source.GetAccess() != RHIAccess::Read) {
        Error = ErrorMessage("Blit-to-swapchain requires a ready readable transfer-source view");
        return;
    }
    const auto& Src = static_cast<const VulkanTexture&>(*SourcePtr);
    if (ToVkImageAspect(Src.GetFormat()) != vk::ImageAspectFlagBits::eColor) {
        Error = ErrorMessage("Blit-to-swapchain requires a color source target");
        return;
    }
    if (Cmd.DstWidth == 0 || Cmd.DstHeight == 0 || Cmd.DstX > m_Swapchain.GetExtent().width ||
        Cmd.DstY > m_Swapchain.GetExtent().height || Cmd.DstWidth > m_Swapchain.GetExtent().width - Cmd.DstX ||
        Cmd.DstHeight > m_Swapchain.GetExtent().height - Cmd.DstY) {
        Error = ErrorMessage("Blit-to-swapchain destination region exceeds the acquired image");
        return;
    }
    const Uint32 Mip = Cmd.Source.GetRange().BaseMipLevel;
    if (Mip >= 32 || Mip >= Src.GetMipLevelCount() ||
        (Cmd.Source.GetRange().MipLevelCount != 1 &&
         (Cmd.Source.GetRange().MipLevelCount != kRHIRemainingSubresources || Mip + 1 != Src.GetMipLevelCount()))) {
        Error = ErrorMessage("Blit-to-swapchain source view must select one valid mip");
        return;
    }
    const Uint32 MipWidth  = (std::max)(Src.GetWidth() >> Mip, 1u);
    const Uint32 MipHeight = (std::max)(Src.GetHeight() >> Mip, 1u);
    const auto   SrcImage  = Src.GetVkImage();
    const auto   DstImage  = m_Swapchain.GetImage(m_Swapchain.GetCurrentIndex());

    // step 1: transition the swapchain image to transfer-dst; it stays backend-private (the
    // graph-declared source is transitioned by the pass's prologue)
    ResourceContext.GetImageTracker().Transition(
        Buf,
        DstImage,
        vk::ImageSubresourceRange{.aspectMask     = vk::ImageAspectFlagBits::eColor,
                                  .baseMipLevel   = 0,
                                  .levelCount     = 1,
                                  .baseArrayLayer = 0,
                                  .layerCount     = 1},
        VulkanImageState{
            .stage   = vk::PipelineStageFlagBits2::eTransfer,
            .access  = vk::AccessFlagBits2::eTransferWrite,
            .layout  = vk::ImageLayout::eTransferDstOptimal,
            .isWrite = true,
        });

    // step 2: blit the whole source into the explicit destination rect; filtering is fixed Nearest
    vk::ImageBlit BlitRegion{
        .srcSubresource = {.aspectMask     = vk::ImageAspectFlagBits::eColor,
                           .mipLevel       = Mip,
                           .baseArrayLayer = 0,
                           .layerCount     = 1},
        .srcOffsets =
            std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0},
                                        vk::Offset3D{static_cast<Int32>(MipWidth), static_cast<Int32>(MipHeight), 1}},
        .dstSubresource = {.aspectMask     = vk::ImageAspectFlagBits::eColor,
                           .mipLevel       = 0,
                           .baseArrayLayer = 0,
                           .layerCount     = 1},
        .dstOffsets =
            std::array<vk::Offset3D, 2>{vk::Offset3D{static_cast<Int32>(Cmd.DstX), static_cast<Int32>(Cmd.DstY), 0},
                                        vk::Offset3D{static_cast<Int32>(Cmd.DstX + Cmd.DstWidth),
                                                     static_cast<Int32>(Cmd.DstY + Cmd.DstHeight),
                                                     1}},
    };
    Buf.blitImage(SrcImage,
                  vk::ImageLayout::eTransferSrcOptimal,
                  DstImage,
                  vk::ImageLayout::eTransferDstOptimal,
                  {BlitRegion},
                  vk::Filter::eNearest);
}

auto VulkanTransferCmdVisitor::operator()(const RHIReadbackTextureCmd& Cmd) -> void {
    const auto* SourcePtr = Cmd.Source.GetTexture().TryGet();
    auto*       TargetPtr = Cmd.Target.TryGet();
    if (!SourcePtr || !TargetPtr || Cmd.Source.GetUsage() != RHITextureUsage::TransferSrc ||
        Cmd.Source.GetAccess() != RHIAccess::Read) {
        Error = ErrorMessage("Texture-readback requires ready resources and a readable transfer-source view");
        return;
    }
    const auto& Src    = static_cast<const VulkanTexture&>(*SourcePtr);
    auto&       Dst    = static_cast<VulkanReadback&>(*TargetPtr);
    const auto  Format = Src.GetFormat();
    if (Format != RHIFormat::R8G8B8A8_UNORM && Format != RHIFormat::B8G8R8A8_UNORM && Format != RHIFormat::R32_SFLOAT &&
        Format != RHIFormat::R32_UINT) {
        Error = ErrorMessage("Texture readback requires a 32-bit color texel format");
        return;
    }
    const Uint32 Mip = Cmd.Source.GetRange().BaseMipLevel;
    if (Mip >= 32 || Mip >= Src.GetMipLevelCount() ||
        (Cmd.Source.GetRange().MipLevelCount != 1 &&
         (Cmd.Source.GetRange().MipLevelCount != kRHIRemainingSubresources || Mip + 1 != Src.GetMipLevelCount()))) {
        Error = ErrorMessage("Texture-readback view must select one valid mip");
        return;
    }
    const Uint32 MipWidth  = (std::max)(Src.GetWidth() >> Mip, 1u);
    const Uint32 MipHeight = (std::max)(Src.GetHeight() >> Mip, 1u);
    if (Cmd.SrcWidth == 0 || Cmd.SrcHeight == 0 || Cmd.SrcX > MipWidth || Cmd.SrcY > MipHeight ||
        Cmd.SrcWidth > MipWidth - Cmd.SrcX || Cmd.SrcHeight > MipHeight - Cmd.SrcY) {
        Error = ErrorMessage("Texture-readback region exceeds the selected mip");
        return;
    }
    if (Cmd.SrcWidth > Dst.GetSize() / sizeof(Uint32) / Cmd.SrcHeight) {
        Error = ErrorMessage("Texture-readback payload exceeds the target size");
        return;
    }
    const auto Offset = Dst.AllocateWriteOffset();

    const vk::BufferImageCopy Region{
        .bufferOffset      = Offset,
        .bufferRowLength   = 0,
        .bufferImageHeight = 0,
        .imageSubresource  = {.aspectMask     = ToVkImageAspect(Src.GetFormat()),
                              .mipLevel       = Mip,
                              .baseArrayLayer = 0,
                              .layerCount     = 1},
        .imageOffset       = {static_cast<Int32>(Cmd.SrcX), static_cast<Int32>(Cmd.SrcY), 0},
        .imageExtent       = {Cmd.SrcWidth, Cmd.SrcHeight, 1},
    };
    Buf.copyImageToBuffer(Src.GetVkImage(), vk::ImageLayout::eTransferSrcOptimal, Dst.GetVkBuffer(), {Region});
}

} // namespace SoulEngine
