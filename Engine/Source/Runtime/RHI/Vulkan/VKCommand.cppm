export module Vulkan:Command;

import Core;
import RHI;
import vulkan;
import std;

import :Types;
import :Context;
import :Capability;
import :Swapchain;
import :Buffer;
import :Pipeline;
import :RayTracingPipeline;
import :Sampler;
import :Texture;
import :Descriptor;
import :AccelerationStructure;
import :ShaderBindingSet;

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

    /// Bind a pipeline-owned shader binding set and apply the committed
    /// dynamic-offset and push-constant snapshot for the current pass.
    auto BindShaderBindingSet(VulkanShaderBindingSet& BindingSet,
                              vk::PipelineBindPoint BindPoint,
                              vk::PipelineStageFlags2 TransitionStages) -> void {
        const vk::PipelineLayout Layout = BindingSet.GetPipelineLayout();
        const auto SetObjects = BindingSet.GetDescriptorSets();
        std::vector<vk::DescriptorSet> Sets;
        Sets.reserve(SetObjects.size());
        for (const auto& Set : SetObjects)
            Sets.push_back(*Set);

        if (BindingSet.HasUnboundBindings()) {
            Error = ErrorMessage(
                Format("Shader binding set '{}' has unbound resources", BindingSet.GetName()));
            return;
        }

        for (const auto& Slot : BindingSet.GetBindings()) {
            if (Slot.Info.Type != ShaderResourceType::SampledTexture &&
                Slot.Info.Type != ShaderResourceType::StorageTexture)
                continue;
            const auto* Ref = std::get_if<RHIRef<RHIRenderTarget>>(&Slot.Resource);
            if (!Ref)
                continue;
            const auto* Target = static_cast<const VulkanRenderTarget*>(Ref->TryGet());
            if (!Target) {
                Error = ErrorMessage(
                    Format("Shader binding '{}' has no ready render target", Slot.Info.ParameterPath));
                return;
            }
            const bool IsSampled = Slot.Info.Type == ShaderResourceType::SampledTexture;
            LocalStates.Transition(
                Buf,
                Target->GetVkImage(),
                VulkanImageState{
                    .stage  = TransitionStages,
                    .access = IsSampled ? vk::AccessFlagBits2::eShaderRead
                                        : vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                    .layout = IsSampled ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eGeneral,
                    .aspect = ToVkImageAspect(Target->GetFormat()),
                });
        }

        BindingSet.Flush();

        auto State = BindingSet.PopCommittedState();
        if (!State) {
            Error = State.error().Append("Failed to resolve shader binding snapshot");
            return;
        }
        if (!Sets.empty())
            Buf.bindDescriptorSets(BindPoint, Layout, 0, Sets, State->DynamicOffsets);
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
    VulkanCmdVisitor(
        vk::raii::CommandBuffer&                                                        InBuffer,
        VulkanImageTracker&                                                             InLocalStates)
        : Buf(InBuffer),
          LocalStates(InLocalStates) {}

    vk::raii::CommandBuffer&                         Buf;
    VulkanImageTracker&                             LocalStates;
    std::optional<ErrorMessage>                      Error                         = std::nullopt;
};

class VulkanGraphicsCmdVisitor final : public VulkanCmdVisitor {
  public:
    using VulkanCmdVisitor::operator();

    ~VulkanGraphicsCmdVisitor() {
        if (m_IsRendering)
            Buf.endRendering();
    }

    VulkanGraphicsCmdVisitor(
        VulkanGraphicsPipeline&                          Pipeline,
        const RHIGraphicsAttachments&                    Attachments,
        vk::raii::CommandBuffer&                         InBuffer,
        VulkanImageTracker&                             InLocalStates)
        : VulkanCmdVisitor(InBuffer,
                           InLocalStates),
          m_Pipeline(Pipeline) {
        Buf.bindPipeline(vk::PipelineBindPoint::eGraphics, Pipeline.Get());
        BindShaderBindingSet(static_cast<VulkanShaderBindingSet&>(*Pipeline.GetShaderBindingSet().TryGet()),
                             vk::PipelineBindPoint::eGraphics,
                             vk::PipelineStageFlagBits2::eAllGraphics);
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
        Uint32 RenderWidth = 0;
        Uint32 RenderHeight = 0;

        for (const auto& AttachmentDesc : Attachments.ColorAttachments) {
            auto* ColorTarget = AttachmentDesc.TextureRef.TryGet();
            if (!ColorTarget) {
                Error = ErrorMessage("Rendering pass color attachment is not ready");
                return;
            }
            const auto& ColorRT = static_cast<const VulkanRenderTarget&>(*ColorTarget);
            LocalStates.Transition(
                Buf,
                ColorRT.GetVkImage(),
                VulkanImageState{
                    .stage  = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                    // Clear uses eClear, which does not load old contents; eLoad must
                    // read the existing color while also writing the attachment.
                    .access = AttachmentDesc.Clear ? vk::AccessFlagBits2::eColorAttachmentWrite
                                                   : vk::AccessFlagBits2::eColorAttachmentRead |
                                                         vk::AccessFlagBits2::eColorAttachmentWrite,
                    .layout = vk::ImageLayout::eColorAttachmentOptimal,
                    .aspect = ToVkImageAspect(ColorRT.GetFormat()),
                    .isWrite = true,
                });
            if (AttachmentDesc.Clear && std::holds_alternative<std::monostate>(AttachmentDesc.ClearValue)) {
                Error = ErrorMessage("Rendering pass color attachment clear value is not set");
                return;
            }
            ColorAttachments.push_back(vk::RenderingAttachmentInfo{
                .imageView   = ColorRT.GetVkImageView(),
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
            auto* DepthTarget = Attachments.DepthAttachment->TextureRef.TryGet();
            if (!DepthTarget) {
                Error = ErrorMessage("Rendering pass depth attachment is not ready");
                return;
            }
            auto& VkDepthRT = static_cast<const VulkanRenderTarget&>(*DepthTarget);
            m_CurrentDepthFormat = VkDepthRT.GetFormat();
            LocalStates.Transition(
                Buf,
                VkDepthRT.GetVkImage(),
                VulkanImageState{
                    // Depth attachment writes, including the final store operation, may
                    // occur in either fragment-test stage. This is synchronization
                    // coverage, not a request to force Early-Z or Late-Z execution.
                    .stage = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                             vk::PipelineStageFlagBits2::eLateFragmentTests,
                    // Depth follows the same clear/load distinction; depth eLoad reads
                    // the existing depth, so the load path needs read and write access.
                    .access = Attachments.DepthAttachment->Clear
                                  ? vk::AccessFlagBits2::eDepthStencilAttachmentWrite
                                  : vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                        vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
                    .layout = vk::ImageLayout::eDepthAttachmentOptimal,
                    .aspect = ToVkImageAspect(VkDepthRT.GetFormat()),
                    .isWrite = true,
                });
            DepthAttachment = vk::RenderingAttachmentInfo{
                .imageView = VkDepthRT.GetVkImageView(),
                .imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
                .loadOp = Attachments.DepthAttachment->Clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
                .storeOp = vk::AttachmentStoreOp::eStore,
                .clearValue = vk::ClearValue{.depthStencil =
                                                  vk::ClearDepthStencilValue{
                                                      Attachments.DepthAttachment->ClearValue.Depth,
                                                      Attachments.DepthAttachment->ClearValue.Stencil}},
            };
            // Clamp render area to smallest attachment dimension.  Vulkan
            // requires every attachment imageView extent ≥ renderArea, so if
            // the depth RT is smaller than the color RT (or vice versa) we
            // must shrink renderArea to fit the minimum.
            m_CurrentRenderExtent.width = (std::min)(m_CurrentRenderExtent.width, VkDepthRT.GetWidth());
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
                        {vk::Viewport{
                            Cmd.X, Cmd.Y + Cmd.Height, Cmd.Width, -Cmd.Height, Cmd.MinDepth, Cmd.MaxDepth}});
    }

    auto operator()(const RHISetScissorCmd& Cmd) -> void {
        Buf.setScissor(0, {vk::Rect2D{{Cmd.X, Cmd.Y}, {Cmd.Width, Cmd.Height}}});
    }

    auto operator()(const RHIDrawIndexedCmd& Cmd) -> void {
        for (Uint32 Binding = 0; Binding < Cmd.VertexBufferRefs.size(); ++Binding) {
            auto* VertexBufferPtr = Cmd.VertexBufferRefs[Binding].TryGet();
            if (!VertexBufferPtr)
                continue;
            const auto& VkVB = static_cast<const VulkanVertexBuffer&>(*VertexBufferPtr);
            Buf.bindVertexBuffers(Binding, {VkVB.GetVkBuffer()}, {0});
        }
        const auto* IndexBufferPtr = Cmd.IndexBufferRef.TryGet();
        if (!IndexBufferPtr) {
            Error = ErrorMessage("Indexed draw is missing its index buffer");
            return;
        }
        const auto& VkIB = static_cast<const VulkanIndexBuffer&>(*IndexBufferPtr);
        Buf.bindIndexBuffer(VkIB.GetVkBuffer(), 0, vk::IndexType::eUint32);
        Buf.drawIndexed(static_cast<Uint32>(VkIB.GetIndexCount()), 1, 0, 0, 0);
    }

    auto operator()(const RHIDrawCmd& Cmd) -> void {
        if (!Cmd.VertexBufferRefs[0].TryGet()) {
            Buf.draw(3, 1, 0, 0);
            return;
        }

        for (Uint32 Binding = 0; Binding < Cmd.VertexBufferRefs.size(); ++Binding) {
            auto* VertexBufferPtr = Cmd.VertexBufferRefs[Binding].TryGet();
            if (!VertexBufferPtr)
                continue;
            const auto& VkVB = static_cast<const VulkanVertexBuffer&>(*VertexBufferPtr);
            Buf.bindVertexBuffers(Binding, {VkVB.GetVkBuffer()}, {0});
        }
        const auto& VkVB = static_cast<const VulkanVertexBuffer&>(*Cmd.VertexBufferRefs[0].TryGet());
        Buf.draw(static_cast<Uint32>(VkVB.GetVertexCount()), 1, 0, 0);
    }

    auto operator()(const RHIDrawIndirectCmd& Cmd) -> void {
        const auto* StorageBuffer = Cmd.IndirectBuffer.TryGet();
        if (!StorageBuffer) {
            Error = ErrorMessage("Indirect draw references an invalid transient storage buffer");
            return;
        }
        const auto& VulkanBuffer = static_cast<const VulkanTransientShaderStorageBuffer&>(*StorageBuffer);
        if (Cmd.Offset > VulkanBuffer.GetSize() || Cmd.Stride < sizeof(Uint32) * 4 ||
            Cmd.DrawCount > (VulkanBuffer.GetSize() - Cmd.Offset) / Cmd.Stride) {
            Error = ErrorMessage("Indirect draw range exceeds its transient buffer");
            return;
        }
        if (Cmd.DrawCount > VulkanCapability::Get().GetProperties().limits.maxDrawIndirectCount) {
            Error = ErrorMessage("Indirect draw count exceeds Vulkan maxDrawIndirectCount");
            return;
        }
        Buf.drawIndirect(VulkanBuffer.GetArenaBuffer(),
                         VulkanBuffer.GetOffset() + Cmd.Offset,
                         Cmd.DrawCount,
                         Cmd.Stride);
    }

  private:
    VulkanGraphicsPipeline& m_Pipeline;
    bool                    m_IsRendering = false;
    vk::Extent2D             m_CurrentRenderExtent = {1, 1};
    std::vector<RHIFormat>   m_CurrentColorFormats = {};
    RHIFormat                m_CurrentDepthFormat = RHIFormat::Unknown;
};

class VulkanRayTracingCmdVisitor final : public VulkanCmdVisitor {
  public:
    using VulkanCmdVisitor::operator();

    VulkanRayTracingCmdVisitor(
        VulkanRayTracingPipeline&                                                      Pipeline,
        vk::raii::CommandBuffer&                                                        InBuffer,
        VulkanImageTracker&                                                             InLocalStates)
        : VulkanCmdVisitor(InBuffer,
                           InLocalStates),
          m_Pipeline(Pipeline) {
        Buf.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, Pipeline.Get());
        BindShaderBindingSet(static_cast<VulkanShaderBindingSet&>(*Pipeline.GetShaderBindingSet().TryGet()),
                             vk::PipelineBindPoint::eRayTracingKHR,
                             vk::PipelineStageFlagBits2::eRayTracingShaderKHR);
    }

    auto operator()(const RHIBuildOrUpdateTopLevelAccelerationStructureCmd& Cmd) -> void {
        auto* TargetPtr = Cmd.TargetRef.TryGet();
        if (!TargetPtr) {
            Error = ErrorMessage("TLAS build references an invalid target");
            return;
        }
        auto& Tlas = static_cast<VulkanTopLevelAccelerationStructure&>(*TargetPtr);
        if (auto R = Tlas.RecordBuild(Buf, Cmd.Instances, Cmd.Mode); !R) {
            Error = R.error().Append("Failed to record TLAS build");
            return;
        }
        const vk::MemoryBarrier2 BuildBarrier{
            .srcStageMask  = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
            .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
            .dstStageMask  = vk::PipelineStageFlagBits2::eRayTracingShaderKHR,
            .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR,
        };
        Buf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &BuildBarrier});
    }

    auto operator()(const RHITraceRaysCmd& Cmd) -> void {
        Buf.traceRaysKHR(m_Pipeline.GetRayGenerationRegion(),
                         m_Pipeline.GetMissRegion(),
                         m_Pipeline.GetHitRegion(),
                         m_Pipeline.GetCallableRegion(),
                         Cmd.Width,
                         Cmd.Height,
                         Cmd.Depth);
        const vk::MemoryBarrier2 TraceBarrier{
            .srcStageMask  = vk::PipelineStageFlagBits2::eRayTracingShaderKHR,
            .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR,
            .dstStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
            .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
        };
        Buf.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &TraceBarrier});
    }

  private:
    VulkanRayTracingPipeline& m_Pipeline;
};


} // namespace SoulEngine
