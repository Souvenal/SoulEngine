export module Vulkan:Command;

import Core;
import RHI;
import vulkan;
import std;

import :Types;
import :Swapchain;
import :Buffer;
import :Pipeline;
import :Texture;
import :Descriptor;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

/// Callable for std::visit over RHI::Command variants.
struct CommandVisitor {
    vk::raii::CommandBuffer&                   Buf;
    std::unordered_map<vk::Image, ImageState>& LocalStates;
    std::array<vk::DescriptorSet, 3>            DescriptorSets            = {};
    vk::Extent2D                               CurrentRenderExtent        = {1, 1};

    /// Begin rendering scope from Pass desc.
    auto BeginPass(const RHI::RenderingDesc& Desc) -> void {
        // ── Resolve color attachment ──────────────────────────────────
        vk::RenderingAttachmentInfo ColorAttachment{};
        vk::ImageView               ColorImageView;
        vk::Image                   ColorImage;
        Uint32                      RenderWidth  = 1;
        Uint32                      RenderHeight = 1;

        auto& VkRT     = static_cast<const Vulkan::RenderTarget&>(*Desc.ColorAttachment.TexturePtr);
        ColorImage     = VkRT.GetVkImage();
        ColorImageView = VkRT.GetVkImageView();
        RenderWidth    = VkRT.GetWidth();
        RenderHeight   = VkRT.GetHeight();
        TransitionImage(Buf,
                        LocalStates,
                        ColorImage,
                        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                        vk::AccessFlagBits2::eColorAttachmentWrite,
                        vk::ImageLayout::eColorAttachmentOptimal,
                        true,
                        ToVkImageAspect(VkRT.GetFormat()));

        ColorAttachment = vk::RenderingAttachmentInfo{
            .imageView   = ColorImageView,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp      = vk::AttachmentLoadOp::eClear,
            .storeOp     = vk::AttachmentStoreOp::eStore,
            .clearValue  = vk::ClearValue{.color = vk::ClearColorValue(std::array<float, 4>{
                                              Desc.ColorAttachment.ClearValue.R,
                                              Desc.ColorAttachment.ClearValue.G,
                                              Desc.ColorAttachment.ClearValue.B,
                                              Desc.ColorAttachment.ClearValue.A,
                                          })},
        };

        CurrentRenderExtent = vk::Extent2D{RenderWidth, RenderHeight};

        // ── Resolve depth attachment (optional) ───────────────────────
        std::optional<vk::RenderingAttachmentInfo> DepthAttachment = std::nullopt;
        if (Desc.DepthAttachment.has_value() && Desc.DepthAttachment->TexturePtr) {
            auto& VkDepthRT   = static_cast<const Vulkan::RenderTarget&>(*Desc.DepthAttachment->TexturePtr);
            auto  DepthImage  = VkDepthRT.GetVkImage();
            auto  DepthView   = VkDepthRT.GetVkImageView();
            auto  DepthAspect = ToVkImageAspect(VkDepthRT.GetFormat());
            TransitionImage(Buf,
                            LocalStates,
                            DepthImage,
                            vk::PipelineStageFlagBits2::eEarlyFragmentTests,
                            vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
                            vk::ImageLayout::eDepthAttachmentOptimal,
                            true,
                            DepthAspect);
            DepthAttachment = vk::RenderingAttachmentInfo{
                .imageView   = DepthView,
                .imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
                .loadOp      = vk::AttachmentLoadOp::eClear,
                .storeOp     = vk::AttachmentStoreOp::eStore,
                .clearValue  = vk::ClearValue{.depthStencil =
                                                  vk::ClearDepthStencilValue{Desc.DepthAttachment->ClearValue.Depth,
                                                                             Desc.DepthAttachment->ClearValue.Stencil}},
            };
        }

        // Clamp render area to smallest attachment dimension.  Vulkan
        // requires every attachment imageView extent ≥ renderArea, so if
        // the depth RT is smaller than the color RT (or vice versa) we
        // must shrink renderArea to fit the minimum.
        if (Desc.DepthAttachment.has_value() && Desc.DepthAttachment->TexturePtr) {
            auto& VkDepthRT            = static_cast<const Vulkan::RenderTarget&>(*Desc.DepthAttachment->TexturePtr);
            CurrentRenderExtent.width  = (std::min)(CurrentRenderExtent.width, VkDepthRT.GetWidth());
            CurrentRenderExtent.height = (std::min)(CurrentRenderExtent.height, VkDepthRT.GetHeight());
        }

        // ── Begin dynamic rendering ───────────────────────────────────
        vk::RenderingInfo RenderingInfo{
            .renderArea           = vk::Rect2D{{0, 0}, CurrentRenderExtent},
            .layerCount           = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments    = &ColorAttachment,
            .pDepthAttachment     = DepthAttachment.has_value() ? &*DepthAttachment : nullptr,
        };
        Buf.beginRendering(RenderingInfo);
    }

    /// End rendering scope.
    auto EndPass() -> void {
        Buf.endRendering();
    }

    auto operator()(const RHI::SetGraphicsPipelineCmd& Cmd) -> void {
        if (!Cmd.PipelinePtr)
            return;

        const auto& Pipeline = static_cast<const Vulkan::GraphicsPipeline&>(*Cmd.PipelinePtr);
        Buf.bindPipeline(vk::PipelineBindPoint::eGraphics, Pipeline.Get());

        const auto SetCount = Pipeline.GetDescriptorSetCount();
        std::vector<vk::DescriptorSet> Sets;
        Sets.reserve(SetCount);
        for (Uint32 Index = 0; Index < SetCount && Index < DescriptorSets.size(); ++Index)
            Sets.push_back(DescriptorSets[Index]);

        std::vector<Uint32> DynamicOffsets(Pipeline.GetDynamicOffsetCount(), 0);
        Buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                               Pipeline.GetPipelineLayout(),
                               0,
                               Sets,
                               DynamicOffsets);
    }

    auto PushDrawParameters(const Vulkan::GraphicsPipeline& Pipeline, const RHI::DrawParameter& Parameters) -> void {
        if (!Parameters.TestTexture || Pipeline.GetPushConstantSize() < sizeof(Uint32))
            return;

        const auto&  VulkanTex = static_cast<const Vulkan::SampledTexture&>(*Parameters.TestTexture);
        const Uint32 Slot      = VulkanTex.GetDescriptorSlot();
        Buf.pushConstants(Pipeline.GetPipelineLayout(),
                          vk::ShaderStageFlagBits::eAllGraphics,
                          0,
                          sizeof(Uint32),
                          &Slot);
    }

    auto operator()(const RHI::SetViewportCmd& Cmd) -> void {
        // Vulkan allows a negative viewport height to flip the framebuffer Y axis.
        vk::Viewport Viewport{
            Cmd.X,
            Cmd.Y + Cmd.Height,
            Cmd.Width,
            -Cmd.Height,
            Cmd.MinDepth,
            Cmd.MaxDepth,
        };
        Buf.setViewport(0, {Viewport});
    }

    auto operator()(const RHI::SetFullViewportCmd& Cmd) -> void {
        // Vulkan allows a negative viewport height to flip the framebuffer Y axis.
        vk::Viewport Viewport{
            0.0f,
            static_cast<Float32>(CurrentRenderExtent.height),
            static_cast<Float32>(CurrentRenderExtent.width),
            -static_cast<Float32>(CurrentRenderExtent.height),
            Cmd.MinDepth,
            Cmd.MaxDepth,
        };
        Buf.setViewport(0, {Viewport});
    }

    auto operator()(const RHI::SetScissorCmd& Cmd) -> void {
        Buf.setScissor(0, {vk::Rect2D{{Cmd.X, Cmd.Y}, {Cmd.Width, Cmd.Height}}});
    }

    auto operator()(const RHI::SetFullScissorRectCmd& /*Cmd*/) -> void {
        Buf.setScissor(0, {vk::Rect2D{{0, 0}, CurrentRenderExtent}});
    }

    auto operator()(const RHI::DrawIndexedCmd& Cmd) -> void {
        if (!Cmd.PipelinePtr || !Cmd.VertexBufferPtr || !Cmd.IndexBufferPtr)
            return;

        const auto& VkPipe = static_cast<const Vulkan::GraphicsPipeline&>(*Cmd.PipelinePtr);
        const auto& VkVB   = static_cast<const Vulkan::VertexBuffer&>(*Cmd.VertexBufferPtr);
        const auto& VkIB   = static_cast<const Vulkan::IndexBuffer&>(*Cmd.IndexBufferPtr);
        PushDrawParameters(VkPipe, Cmd.Parameters);
        Buf.bindVertexBuffers(0, {VkVB.GetVkBuffer()}, {0});
        Buf.bindIndexBuffer(VkIB.GetVkBuffer(), 0, vk::IndexType::eUint32);
        Buf.drawIndexed(static_cast<Uint32>(VkIB.GetIndexCount()), 1, 0, 0, 0);
    }

    auto operator()(const RHI::DrawCmd& Cmd) -> void {
        if (!Cmd.PipelinePtr || !Cmd.VertexBufferPtr)
            return;

        const auto& VkPipe = static_cast<const Vulkan::GraphicsPipeline&>(*Cmd.PipelinePtr);
        const auto& VkVB   = static_cast<const Vulkan::VertexBuffer&>(*Cmd.VertexBufferPtr);
        PushDrawParameters(VkPipe, Cmd.Parameters);
        Buf.bindVertexBuffers(0, {VkVB.GetVkBuffer()}, {0});
        Buf.draw(static_cast<Uint32>(VkVB.GetVertexCount()), 1, 0, 0);
    }
};

} // namespace SoulEngine::RHI::Vulkan
