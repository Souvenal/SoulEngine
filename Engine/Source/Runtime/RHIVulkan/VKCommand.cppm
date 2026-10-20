export module RHIVulkan:Command;

import Core;
import RHI;
import vulkan;
import std;

import :Types;
import :Context;
import :Capability;
import :Swapchain;
import :Buffer;
import :Readback;
import :Pipeline;
import :RayTracingPipeline;
import :Sampler;
import :Texture;
import :Descriptor;
import :AccelerationStructure;
import :ShaderBindingSet;
import Shader;

namespace SoulEngine {

[[nodiscard]] static auto ToVkClearColor(const RHIClearColorValue& Value) -> vk::ClearColorValue {
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

/// Shared Vulkan command execution context and operations.
class VulkanCmdVisitor {
  public:
    VulkanCmdVisitor(const VulkanCmdVisitor&)                    = delete;
    auto operator=(const VulkanCmdVisitor&) -> VulkanCmdVisitor& = delete;
    VulkanCmdVisitor(VulkanCmdVisitor&&)                         = delete;
    auto operator=(VulkanCmdVisitor&&) -> VulkanCmdVisitor&      = delete;
    virtual ~VulkanCmdVisitor()                                  = default;

    template <typename T>
    auto operator()(const T&) -> void {
        Error = ErrorMessage("Command is not supported by this Vulkan pass visitor");
    }

    /// @brief Bind a pipeline-owned shader binding set for the current pass.
    ///
    /// Applies the committed dynamic-offset and push-constant snapshot.
    auto BindShaderBindingSet(VulkanShaderBindingSet& BindingSet, vk::PipelineBindPoint BindPoint) -> void {
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

    [[nodiscard]] auto GetError() const -> const std::optional<ErrorMessage>& {
        return Error;
    }

  protected:
    VulkanCmdVisitor(vk::raii::CommandBuffer& InBuffer, VulkanResourceContext& InResourceContext)
        : Buf(InBuffer), ResourceContext(InResourceContext) {}

    vk::raii::CommandBuffer&    Buf;
    VulkanResourceContext&      ResourceContext;
    std::optional<ErrorMessage> Error = std::nullopt;
};

class VulkanGraphicsCmdVisitor final : public VulkanCmdVisitor {
  public:
    using VulkanCmdVisitor::operator();

    ~VulkanGraphicsCmdVisitor() {
        if (m_IsRendering)
            Buf.endRendering();
    }

    VulkanGraphicsCmdVisitor(VulkanGraphicsPipeline&       Pipeline,
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

    auto BeginRendering(const RHIGraphicsAttachments& Attachments) -> void {
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
            auto* ColorTarget = AttachmentDesc.View.Texture.TryGet();
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
                .imageView   = AttachmentDesc.View.Range.MipLevelCount == kRHIRemainingSubresources
                                   ? ColorRT.GetVkImageView()
                                   : ColorRT.GetVkImageView(AttachmentDesc.View.Range.BaseMipLevel),
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
            auto* DepthTarget = Attachments.DepthAttachment->View.Texture.TryGet();
            if (!DepthTarget) {
                Error = ErrorMessage("Rendering pass depth attachment is not ready");
                return;
            }
            auto& VkDepthRT      = static_cast<const VulkanTexture&>(*DepthTarget);
            m_CurrentDepthFormat = VkDepthRT.GetFormat();
            DepthAttachment      = vk::RenderingAttachmentInfo{
                .imageView   = Attachments.DepthAttachment->View.Range.MipLevelCount == kRHIRemainingSubresources
                                   ? VkDepthRT.GetVkImageView()
                                   : VkDepthRT.GetVkImageView(Attachments.DepthAttachment->View.Range.BaseMipLevel),
                .imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
                .loadOp =
                    Attachments.DepthAttachment->Clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
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

        if (!m_CurrentColorFormats.empty() &&
            !m_Pipeline.IsCompatibleWith(m_CurrentColorFormats, m_CurrentDepthFormat)) {
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

    auto operator()(const RHISetViewportCmd& Cmd) -> void {
        Buf.setViewport(0,
                        {vk::Viewport{Cmd.X, Cmd.Y + Cmd.Height, Cmd.Width, -Cmd.Height, Cmd.MinDepth, Cmd.MaxDepth}});
    }

    auto operator()(const RHISetScissorCmd& Cmd) -> void {
        Buf.setScissor(0, {vk::Rect2D{{Cmd.X, Cmd.Y}, {Cmd.Width, Cmd.Height}}});
    }

    auto operator()(const RHIDrawCmd& Cmd) -> void {
        Buf.draw(Cmd.VertexCount, 1, 0, 0);
    }

    auto operator()(const RHIDrawIndirectCmd& Cmd) -> void {
        const auto* StorageBuffer = Cmd.IndirectBuffer.TryGet();
        if (!StorageBuffer) {
            Error = ErrorMessage("Indirect draw references an invalid transient storage buffer");
            return;
        }
        const auto& Indirect = static_cast<const VulkanBuffer&>(*StorageBuffer);
        if (Cmd.Offset > Indirect.GetSize() || Cmd.Stride < sizeof(Uint32) * 4 ||
            Cmd.DrawCount > (Indirect.GetSize() - Cmd.Offset) / Cmd.Stride) {
            Error = ErrorMessage("Indirect draw range exceeds its transient buffer");
            return;
        }
        if (Cmd.DrawCount > VulkanCapability::Get().GetProperties().limits.maxDrawIndirectCount) {
            Error = ErrorMessage("Indirect draw count exceeds Vulkan maxDrawIndirectCount");
            return;
        }
        const auto* Prop = Indirect.GetProp<TransientBufferProp>();
        if (!Prop) {
            Error = ErrorMessage("Indirect draw buffer has no transient state");
            return;
        }
        Buf.drawIndirect(Indirect.GetVkBuffer(), Prop->Allocation.Offset + Cmd.Offset, Cmd.DrawCount, Cmd.Stride);
    }

  private:
    VulkanGraphicsPipeline& m_Pipeline;
    bool                    m_IsRendering         = false;
    vk::Extent2D            m_CurrentRenderExtent = {1, 1};
    std::vector<RHIFormat>  m_CurrentColorFormats = {};
    RHIFormat               m_CurrentDepthFormat  = RHIFormat::Unknown;
};

class VulkanComputeCmdVisitor final : public VulkanCmdVisitor {
  public:
    using VulkanCmdVisitor::operator();

    VulkanComputeCmdVisitor(VulkanComputePipeline&   Pipeline,
                            vk::raii::CommandBuffer& InBuffer,
                            VulkanResourceContext&   InResourceContext)
        : VulkanCmdVisitor(InBuffer, InResourceContext), m_Pipeline(Pipeline) {
        Buf.bindPipeline(vk::PipelineBindPoint::eCompute, Pipeline.Get());
        auto* BindingSet = Pipeline.GetShaderBindingSet().TryGet();
        if (BindingSet)
            BindShaderBindingSet(static_cast<VulkanShaderBindingSet&>(*BindingSet), vk::PipelineBindPoint::eCompute);
    }

    auto operator()(const RHIDispatchCmd& Cmd) -> void {
        Buf.dispatch(Cmd.GroupCountX, Cmd.GroupCountY, Cmd.GroupCountZ);
    }

  private:
    VulkanComputePipeline& m_Pipeline;
};

class VulkanRayTracingCmdVisitor final : public VulkanCmdVisitor {
  public:
    using VulkanCmdVisitor::operator();

    VulkanRayTracingCmdVisitor(VulkanRayTracingPipeline& Pipeline,
                               vk::raii::CommandBuffer&  InBuffer,
                               VulkanResourceContext&    InResourceContext)
        : VulkanCmdVisitor(InBuffer, InResourceContext), m_Pipeline(Pipeline) {
        Buf.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, Pipeline.Get());
        auto* BindingSet = Pipeline.GetShaderBindingSet().TryGet();
        if (BindingSet)
            BindShaderBindingSet(static_cast<VulkanShaderBindingSet&>(*BindingSet),
                                 vk::PipelineBindPoint::eRayTracingKHR);
    }

    auto operator()(const RHITraceRaysCmd& Cmd) -> void {
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

  private:
    VulkanRayTracingPipeline& m_Pipeline;
};

class VulkanTransferCmdVisitor final : public VulkanCmdVisitor {
  public:
    using VulkanCmdVisitor::operator();

    VulkanTransferCmdVisitor(vk::raii::CommandBuffer& InBuffer,
                             VulkanResourceContext&   InResourceContext,
                             VulkanSwapchain&         InSwapchain)
        : VulkanCmdVisitor(InBuffer, InResourceContext), m_Swapchain(InSwapchain) {}

    auto operator()(const RHIBlitToSwapchainCmd& Cmd) -> void {
        const auto* SourcePtr = Cmd.Source.TryGet();
        if (!SourcePtr) {
            Error = ErrorMessage("Blit-to-swapchain references a source that is not ready");
            return;
        }
        const auto& Src = static_cast<const VulkanTexture&>(*SourcePtr);
        if (ToVkImageAspect(Src.GetFormat()) != vk::ImageAspectFlagBits::eColor) {
            Error = ErrorMessage("Blit-to-swapchain requires a color source target");
            return;
        }
        const auto SrcImage = Src.GetVkImage();
        const auto DstImage = m_Swapchain.GetImage(m_Swapchain.GetCurrentIndex());

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
                               .mipLevel       = 0,
                               .baseArrayLayer = 0,
                               .layerCount     = 1},
            .srcOffsets =
                std::array<vk::Offset3D, 2>{
                    vk::Offset3D{0, 0, 0},
                    vk::Offset3D{static_cast<Int32>(Src.GetWidth()), static_cast<Int32>(Src.GetHeight()), 1}},
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

    auto operator()(const RHIReadbackTextureCmd& Cmd) -> void {
        const auto* SourcePtr = Cmd.Source.TryGet();
        auto*       TargetPtr = Cmd.Target.TryGet();
        if (!SourcePtr || !TargetPtr) {
            Error = ErrorMessage("Texture-readback references resources that are not ready");
            return;
        }
        const auto& Src = static_cast<const VulkanTexture&>(*SourcePtr);
        auto&       Dst = static_cast<VulkanReadback&>(*TargetPtr);
        if (Cmd.SrcX + Cmd.SrcWidth > Src.GetWidth() || Cmd.SrcY + Cmd.SrcHeight > Src.GetHeight()) {
            Error = ErrorMessage("Texture-readback source region exceeds the source target");
            return;
        }
        // The picking contract keeps payloads at one 32-bit texel; larger regions must stay within the slot.
        const Uint64 PayloadBytes = static_cast<Uint64>(Cmd.SrcWidth) * Cmd.SrcHeight * sizeof(Uint32);
        if (PayloadBytes > Dst.GetSize()) {
            Error = ErrorMessage("Texture-readback payload exceeds the target size");
            return;
        }
        const auto Offset = Dst.AllocateWriteOffset();

        const vk::BufferImageCopy Region{
            .bufferOffset      = Offset,
            .bufferRowLength   = 0,
            .bufferImageHeight = 0,
            .imageSubresource  = {.aspectMask     = ToVkImageAspect(Src.GetFormat()),
                                  .mipLevel       = 0,
                                  .baseArrayLayer = 0,
                                  .layerCount     = 1},
            .imageOffset       = {static_cast<Int32>(Cmd.SrcX), static_cast<Int32>(Cmd.SrcY), 0},
            .imageExtent       = {Cmd.SrcWidth, Cmd.SrcHeight, 1},
        };
        Buf.copyImageToBuffer(Src.GetVkImage(), vk::ImageLayout::eTransferSrcOptimal, Dst.GetVkBuffer(), {Region});
    }

  private:
    VulkanSwapchain& m_Swapchain;
};

} // namespace SoulEngine
