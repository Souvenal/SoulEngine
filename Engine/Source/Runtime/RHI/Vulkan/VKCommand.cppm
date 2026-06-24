export module Vulkan:Command;

import Core;
import RHI;
import vulkan;
import std;

import :Types;
import :Swapchain;
import :Buffer;
import :Pipeline;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

/// Callable for std::visit over RHI::Command variants.
struct CommandVisitor {
    vk::raii::CommandBuffer&                   Buf;
    std::unordered_map<vk::Image, ImageState>& LocalStates;
    Swapchain*                                 Swc;
    vk::Extent2D                               CurrentRenderExtent = {1, 1};

    auto TransitionImage(vk::Image               Image,
                         vk::PipelineStageFlags2 DstStage,
                         vk::AccessFlags2        DstAccess,
                         vk::ImageLayout         DstLayout,
                         bool                    IsWrite,
                         vk::ImageAspectFlags    Aspect = vk::ImageAspectFlagBits::eColor) -> void {
        auto It      = LocalStates.find(Image);
        auto Current = (It != LocalStates.end()) ? It->second : ImageState{};

        bool NeedsBarrier = (Current.stage != DstStage) || (Current.access != DstAccess) ||
                            (Current.layout != DstLayout) || Current.isWrite;

        if (NeedsBarrier) {
            vk::ImageMemoryBarrier2 Barrier{
                .srcStageMask        = Current.stage,
                .srcAccessMask       = Current.access,
                .dstStageMask        = DstStage,
                .dstAccessMask       = DstAccess,
                .oldLayout           = Current.layout,
                .newLayout           = DstLayout,
                .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                .image               = Image,
                .subresourceRange    = {.aspectMask     = Aspect,
                                        .baseMipLevel   = 0,
                                        .levelCount     = vk::RemainingMipLevels,
                                        .baseArrayLayer = 0,
                                        .layerCount     = vk::RemainingArrayLayers},
            };
            vk::DependencyInfo Dep{
                .dependencyFlags         = vk::DependencyFlagBits::eByRegion,
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers    = &Barrier,
            };
            Buf.pipelineBarrier2(Dep);
        }

        LocalStates[Image] = ImageState{
            .stage       = DstStage,
            .access      = DstAccess,
            .layout      = DstLayout,
            .queueFamily = vk::QueueFamilyIgnored,
            .isWrite     = IsWrite,
        };
    }

    /// Begin rendering scope from Pass desc.
    auto BeginPass(const RHI::RenderingDesc& Desc) -> void {
        vk::ImageView ImageView;
        if (!Desc.ColorAttachment.TexturePtr) {
            if (!Swc)
                return;
            const auto CurrentIndex = Swc->GetCurrentIndex();
            const auto CurrentImage = Swc->GetImage(CurrentIndex);
            ImageView               = Swc->GetImageView(CurrentIndex);
            TransitionImage(CurrentImage,
                            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                            vk::AccessFlagBits2::eColorAttachmentWrite,
                            vk::ImageLayout::eColorAttachmentOptimal,
                            true);
        } else {
            return;
        }
        CurrentRenderExtent = Swc->GetExtent();
        vk::RenderingAttachmentInfo ColorAttachment{
            .imageView   = ImageView,
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
        vk::RenderingInfo RenderingInfo{
            .renderArea           = vk::Rect2D{{0, 0}, CurrentRenderExtent},
            .layerCount           = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments    = &ColorAttachment,
        };
        Buf.beginRendering(RenderingInfo);
    }

    /// End rendering scope and transition swapchain to PresentSrc.
    auto EndPass() -> void {
        Buf.endRendering();
        if (Swc)
            TransitionImage(Swc->GetImage(Swc->GetCurrentIndex()),
                            vk::PipelineStageFlagBits2::eBottomOfPipe,
                            vk::AccessFlagBits2::eNone,
                            vk::ImageLayout::ePresentSrcKHR,
                            false);
    }

    auto operator()(const RHI::SetPipelineCmd& Cmd) -> void {
        auto& VkPipe = static_cast<const Vulkan::GraphicsPipeline&>(*Cmd.Pipeline);
        Buf.bindPipeline(vk::PipelineBindPoint::eGraphics, VkPipe.Get());
    }

    auto operator()(const RHI::BindVertexBufferCmd& Cmd) -> void {
        auto& VkBuf = static_cast<const Vulkan::VertexBuffer&>(*Cmd.Buffer);
        Buf.bindVertexBuffers(0, {VkBuf.GetVkBuffer()}, {Cmd.Offset});
    }

    auto operator()(const RHI::BindIndexBufferCmd& Cmd) -> void {
        auto& VkBuf = static_cast<const Vulkan::IndexBuffer&>(*Cmd.Buffer);
        Buf.bindIndexBuffer(VkBuf.GetVkBuffer(), Cmd.Offset, vk::IndexType::eUint32);
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
        Buf.drawIndexed(Cmd.IndexCount, Cmd.InstanceCount, Cmd.FirstIndex, Cmd.VertexOffset, Cmd.FirstInstance);
    }

    auto operator()(const RHI::DrawCmd& Cmd) -> void {
        Buf.draw(Cmd.VertexCount, Cmd.InstanceCount, Cmd.FirstVertex, Cmd.FirstInstance);
    }

    auto operator()(const RHI::SetSampledTextureCmd& /*Cmd*/) -> void {
        // No-op: texture is already in the bindless descriptor set at creation time.
        // The renderer uses push constants to pass the descriptor slot index to the shader.
    }
};

} // namespace SoulEngine::RHI::Vulkan
