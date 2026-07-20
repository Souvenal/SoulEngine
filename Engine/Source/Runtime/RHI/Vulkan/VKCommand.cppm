export module Vulkan:Command;

import Core;
import RHI;
import vulkan;
import std;

import :Types;
import :Swapchain;
import :Buffer;
import :Pipeline;
import :Sampler;
import :Texture;
import :Descriptor;

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

/// Callable for std::visit over RHI::Command variants.
struct CommandVisitor {
    vk::raii::CommandBuffer&                   Buf;
    std::unordered_map<vk::Image, ImageState>& LocalStates;
    DescriptorManager*                         Descriptors                = nullptr;
    UniformBufferArena*                        ConstantArena              = nullptr;
    Uint32                                     FrameIndex                 = 0;
    vk::Extent2D                               CurrentRenderExtent        = {1, 1};
    std::optional<ErrorMessage>                Error                      = std::nullopt;

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

        auto& Pipeline = static_cast<Vulkan::GraphicsPipeline&>(*Cmd.PipelinePtr);
        Buf.bindPipeline(vk::PipelineBindPoint::eGraphics, Pipeline.Get());
    }

    auto operator()(const RHI::PushConstantsCmd& Cmd) -> void {
        if (!Cmd.PipelinePtr || Cmd.Data.empty())
            return;

        auto& Pipeline = static_cast<Vulkan::GraphicsPipeline&>(*Cmd.PipelinePtr);
        if (Cmd.Offset + Cmd.Data.size() > Pipeline.GetPushConstantSize()) {
            Error = ErrorMessage(Core::Format("PushConstants range exceeds reflected pipeline push-constant size ({} + {} > {})",
                                              Cmd.Offset,
                                              Cmd.Data.size(),
                                              Pipeline.GetPushConstantSize()));
            return;
        }

        Buf.pushConstants(Pipeline.GetPipelineLayout(),
                          vk::ShaderStageFlagBits::eAllGraphics,
                          Cmd.Offset,
                          static_cast<Uint32>(Cmd.Data.size()),
                          Cmd.Data.data());
    }

    auto operator()(const RHI::BindShaderParametersCmd& Cmd) -> void {
        if (!Cmd.PipelinePtr)
            return;
        if (!Descriptors || !ConstantArena) {
            Error = ErrorMessage("CommandVisitor: descriptor manager or constant arena is missing");
            return;
        }

        auto& Pipeline = static_cast<Vulkan::GraphicsPipeline&>(*Cmd.PipelinePtr);
        if (Cmd.Parameters.GetLayoutId() != Pipeline.GetShaderParameterLayout().GetId()) {
            Error = ErrorMessage("Shader parameters were created from a different pipeline layout");
            return;
        }

        for (const auto& Parameters : Cmd.Parameters.GetSets()) {
            const auto& Layout   = Parameters.GetLayout();
            const auto  SetIndex = Layout.GetSetIndex();
            const auto  Bindings = Layout.GetBindings();
            const auto  Values   = Parameters.GetValues();

            Uint32 VariableDescriptorCount = 1;
            for (Uint32 Index = 0; Index < Bindings.size(); ++Index) {
                const auto& Binding = Bindings[Index];
                if (Binding.ArrayCount != std::numeric_limits<Uint32>::max())
                    continue;
                if (Binding.Type != Shader::ResourceType::SampledTexture) {
                    Error = ErrorMessage(Core::Format(
                        "Runtime resource array '{}' uses unsupported resource type",
                        Binding.ParameterPath));
                    return;
                }
                const auto* Array = std::get_if<RHI::ResourceArray<RHI::SampledTexture>>(&Values[Index]);
                if (!Array || Array->GetSize() == 0) {
                    Error = ErrorMessage(Core::Format(
                        "Runtime sampled texture array '{}' is missing a non-empty resource array",
                        Binding.ParameterPath));
                    return;
                }
                VariableDescriptorCount = Array->GetSize();
            }

            auto Instance = Pipeline.GetOrCreateDescriptorSetInstance(
                Cmd.Parameters.GetId(), SetIndex, VariableDescriptorCount, *Descriptors);
            if (!Instance) {
                Error = Instance.error();
                return;
            }

            const bool bUpdateDescriptors = !(*Instance)->Initialized ||
                                            (*Instance)->ParameterRevision != Parameters.GetRevision();
            std::vector<std::pair<Uint32, Uint32>> DynamicOffsetWrites;
            for (Uint32 Index = 0; Index < Bindings.size(); ++Index) {
                const auto& Binding = Bindings[Index];
                const auto& Value   = Values[Index];

                if (const auto* Constant = std::get_if<RHI::ShaderParameterConstant>(&Value)) {
                    if (!Constant->Buffer || Constant->Data.empty()) {
                        Error = ErrorMessage(Core::Format(
                            "Shader parameter '{}' has an invalid constant buffer value", Binding.ParameterPath));
                        return;
                    }
                    auto& VkBuffer = static_cast<const Vulkan::ConstantBuffer&>(*Constant->Buffer);
                    const auto Offset = VkBuffer.GetArenaOffset(FrameIndex);
                    if (auto R = ConstantArena->Write(Constant->Data.data(), Constant->Data.size(), Offset); !R) {
                        Error = R.error().Append("Shader parameter constant arena write failed");
                        return;
                    }
                    auto& ResourceBindings = (*Instance)->ResourceBindings;
                    if (!(*Instance)->Initialized ||
                        ResourceBindings[Binding.Binding] != Constant->Buffer) {
                        Descriptors->WriteConstantDescriptor(
                            *(*Instance)->Set, Binding.Binding, *ConstantArena, Constant->Buffer->GetSize());
                        ResourceBindings[Binding.Binding] = Constant->Buffer;
                    }
                    DynamicOffsetWrites.push_back({Binding.Binding, Offset});
                    continue;
                }

                if (!bUpdateDescriptors)
                    continue;

                if (const auto* Texture = std::get_if<RHI::SampledTexture*>(&Value)) {
                    if (!*Texture) {
                        Error = ErrorMessage(Core::Format(
                            "Shader parameter '{}' has a null sampled texture", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkTexture = static_cast<const Vulkan::SampledTexture&>(**Texture);
                    auto& ResourceBindings = (*Instance)->ResourceBindings;
                    if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *Texture) {
                        Descriptors->WriteSampledTextureDescriptor(*(*Instance)->Set,
                                                                   Binding.Binding,
                                                                   0,
                                                                   VkTexture.GetVkImageView(),
                                                                   vk::ImageLayout::eShaderReadOnlyOptimal);
                        ResourceBindings[Binding.Binding] = *Texture;
                    }
                    continue;
                }

                if (const auto* Array = std::get_if<RHI::ResourceArray<RHI::SampledTexture>>(&Value)) {
                    if (Array->GetSize() == 0) {
                        Error = ErrorMessage(Core::Format(
                            "Shader parameter '{}' has an empty sampled-texture resource array",
                            Binding.ParameterPath));
                        return;
                    }
                    const bool bRuntimeArray = Binding.ArrayCount == std::numeric_limits<Uint32>::max();
                    if (!bRuntimeArray && Array->GetSize() != Binding.ArrayCount) {
                        Error = ErrorMessage(Core::Format(
                            "Shader parameter '{}' sampled-texture array size {} does not match reflected array count {}",
                            Binding.ParameterPath,
                            Array->GetSize(),
                            Binding.ArrayCount));
                        return;
                    }
                    for (Uint32 Slot = 0; Slot < Array->GetResources().size(); ++Slot) {
                        auto* Texture = Array->GetResources()[Slot];
                        if (!Texture && bRuntimeArray)
                            continue;
                        if (!Texture) {
                            Error = ErrorMessage(Core::Format(
                                "Fixed-size shader parameter '{}' has an unset sampled-texture array slot {}",
                                Binding.ParameterPath,
                                Slot));
                            return;
                        }
                        const auto& VkTexture = static_cast<const Vulkan::SampledTexture&>(*Texture);
                        Descriptors->WriteSampledTextureDescriptor(*(*Instance)->Set,
                                                                   Binding.Binding,
                                                                   Slot,
                                                                   VkTexture.GetVkImageView(),
                                                                   vk::ImageLayout::eShaderReadOnlyOptimal);
                    }
                    continue;
                }

                if (const auto* Sampler = std::get_if<RHI::Sampler*>(&Value)) {
                    if (!*Sampler) {
                        Error = ErrorMessage(Core::Format(
                            "Shader parameter '{}' has a null sampler", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkSampler = static_cast<const Vulkan::Sampler&>(**Sampler);
                    auto& ResourceBindings = (*Instance)->ResourceBindings;
                    if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *Sampler) {
                        Descriptors->WriteSamplerDescriptor(
                            *(*Instance)->Set, Binding.Binding, VkSampler.GetVkSampler());
                        ResourceBindings[Binding.Binding] = *Sampler;
                    }
                    continue;
                }

                LogWarning("Shader parameter '{}' is unset", Binding.ParameterPath);
            }

            if (bUpdateDescriptors) {
                (*Instance)->ParameterRevision = Parameters.GetRevision();
                (*Instance)->Initialized       = true;
            }

            std::ranges::sort(DynamicOffsetWrites, {}, &std::pair<Uint32, Uint32>::first);
            std::vector<Uint32> DynamicOffsets;
            DynamicOffsets.reserve(DynamicOffsetWrites.size());
            for (const auto& [Binding, Offset] : DynamicOffsetWrites)
                DynamicOffsets.push_back(Offset);

            Buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                   Pipeline.GetPipelineLayout(),
                                   SetIndex,
                                   std::array{*(*Instance)->Set},
                                   DynamicOffsets);
        }
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
        if (!Cmd.PipelinePtr || !Cmd.VertexBuffers[0] || !Cmd.IndexBufferPtr)
            return;

        const auto& VkIB   = static_cast<const Vulkan::IndexBuffer&>(*Cmd.IndexBufferPtr);
        for (Uint32 Binding = 0; Binding < Cmd.VertexBuffers.size(); ++Binding) {
            auto* VertexBufferPtr = Cmd.VertexBuffers[Binding];
            if (!VertexBufferPtr)
                continue;
            const auto& VkVB = static_cast<const Vulkan::VertexBuffer&>(*VertexBufferPtr);
            Buf.bindVertexBuffers(Binding, {VkVB.GetVkBuffer()}, {0});
        }
        Buf.bindIndexBuffer(VkIB.GetVkBuffer(), 0, vk::IndexType::eUint32);
        Buf.drawIndexed(static_cast<Uint32>(VkIB.GetIndexCount()), 1, 0, 0, 0);
    }

    auto operator()(const RHI::DrawCmd& Cmd) -> void {
        if (!Cmd.PipelinePtr || !Cmd.VertexBuffers[0])
            return;

        for (Uint32 Binding = 0; Binding < Cmd.VertexBuffers.size(); ++Binding) {
            auto* VertexBufferPtr = Cmd.VertexBuffers[Binding];
            if (!VertexBufferPtr)
                continue;
            const auto& VkVB = static_cast<const Vulkan::VertexBuffer&>(*VertexBufferPtr);
            Buf.bindVertexBuffers(Binding, {VkVB.GetVkBuffer()}, {0});
        }
        const auto& VkVB = static_cast<const Vulkan::VertexBuffer&>(*Cmd.VertexBuffers[0]);
        Buf.draw(static_cast<Uint32>(VkVB.GetVertexCount()), 1, 0, 0);
    }
};

} // namespace SoulEngine::RHI::Vulkan
