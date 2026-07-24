export module Vulkan:Command;

import Core;
import RHI;
import vulkan;
import std;

import :Types;
import :Swapchain;
import :Buffer;
import :Pipeline;
import :RayTracingPipeline;
import :RayTracingGeometryTable;
import :Sampler;
import :Texture;
import :Descriptor;
import :AccelerationStructure;

namespace SoulEngine {

/// Callable for std::visit over RHICommand variants.
struct VulkanCommandVisitor {
    vk::raii::CommandBuffer&                   Buf;
    std::unordered_map<vk::Image, VulkanImageState>& LocalStates;
    VulkanDescriptorManager*                         Descriptors                = nullptr;
    VulkanUniformBufferArena*                        ConstantArena              = nullptr;
    VulkanTransientUniformBufferArena*               DrawConstantArena          = nullptr;
    Uint32                                     FrameIndex                 = 0;
    vk::Extent2D                               CurrentRenderExtent        = {1, 1};
    enum class BoundPipelineType : Uint8 {
        Unknown = 0,
        Graphics,
        RayTracing,
    };

    std::optional<ErrorMessage>                Error                      = std::nullopt;
    RHIPipeline*                             m_BoundPipeline            = nullptr;
    BoundPipelineType                          m_BoundPipelineType        = BoundPipelineType::Unknown;

    /// Begin rendering scope from RHIPass desc.
    auto BeginPass(const RHIRenderingDesc& Desc) -> void {
        // ── Resolve color attachment ──────────────────────────────────
        vk::RenderingAttachmentInfo ColorAttachment{};
        vk::ImageView               ColorImageView;
        vk::Image                   ColorImage;
        Uint32                      RenderWidth  = 1;
        Uint32                      RenderHeight = 1;

        auto& VkRT     = static_cast<const VulkanRenderTarget&>(*Desc.ColorAttachment.TexturePtr);
        ColorImage     = VkRT.GetVkImage();
        ColorImageView = VkRT.GetVkImageView();
        RenderWidth    = VkRT.GetWidth();
        RenderHeight   = VkRT.GetHeight();
        VulkanTransitionImage(Buf,
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
            auto& VkDepthRT   = static_cast<const VulkanRenderTarget&>(*Desc.DepthAttachment->TexturePtr);
            auto  DepthImage  = VkDepthRT.GetVkImage();
            auto  DepthView   = VkDepthRT.GetVkImageView();
            auto  DepthAspect = ToVkImageAspect(VkDepthRT.GetFormat());
            VulkanTransitionImage(Buf,
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
            auto& VkDepthRT            = static_cast<const VulkanRenderTarget&>(*Desc.DepthAttachment->TexturePtr);
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

    template <typename PipelineType>
    auto BindShaderParameters(const RHIBindShaderParametersCmd& Cmd,
                              PipelineType&                       Pipeline,
                              vk::PipelineBindPoint                BindPoint,
                              vk::PipelineStageFlags2              ShaderStage) -> void {
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
                if (Binding.Type != ShaderResourceType::SampledTexture) {
                    Error = ErrorMessage(Format(
                        "Runtime resource array '{}' uses unsupported resource type",
                        Binding.ParameterPath));
                    return;
                }
                const auto* Array = std::get_if<RHIResourceArray<RHISampledTexture>>(&Values[Index]);
                if (!Array || Array->GetSize() == 0) {
                    Error = ErrorMessage(Format(
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

                if (const auto* Constant = std::get_if<RHIShaderParameterConstant>(&Value)) {
                    if (!Constant->Buffer || Constant->Data.empty()) {
                        Error = ErrorMessage(Format(
                            "Shader parameter '{}' has an invalid constant buffer value", Binding.ParameterPath));
                        return;
                    }
                    auto& VkBuffer = static_cast<const VulkanConstantBuffer&>(*Constant->Buffer);
                    Uint32 Offset = 0;
                    vk::Buffer DescriptorBuffer = nullptr;
                    const void* DescriptorSource = Constant->Buffer;
                    if (Constant->bPerDraw) {
                        auto DrawOffset = DrawConstantArena->Allocate(FrameIndex, Constant->Data.size());
                        if (!DrawOffset) {
                            Error = DrawOffset.error().Append("Shader parameter draw constant allocation failed");
                            return;
                        }
                        Offset = *DrawOffset;
                        if (auto R = DrawConstantArena->Write(Constant->Data.data(), Constant->Data.size(), Offset); !R) {
                            Error = R.error().Append("Shader parameter draw constant arena write failed");
                            return;
                        }
                        DescriptorBuffer = DrawConstantArena->GetVkBuffer();
                        DescriptorSource = DrawConstantArena;
                    } else {
                        Offset = VkBuffer.GetArenaOffset(FrameIndex);
                        if (auto R = ConstantArena->Write(Constant->Data.data(), Constant->Data.size(), Offset); !R) {
                            Error = R.error().Append("Shader parameter constant arena write failed");
                            return;
                        }
                        DescriptorBuffer = ConstantArena->GetVkBuffer();
                    }
                    auto& ResourceBindings = (*Instance)->ResourceBindings;
                    if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != DescriptorSource) {
                        Descriptors->WriteConstantDescriptor(
                            *(*Instance)->Set, Binding.Binding, DescriptorBuffer, Constant->Buffer->GetSize());
                        ResourceBindings[Binding.Binding] = DescriptorSource;
                    }
                    DynamicOffsetWrites.push_back({Binding.Binding, Offset});
                    continue;
                }

                if (const auto* Target = std::get_if<RHIRenderTarget*>(&Value)) {
                    if (!*Target) {
                        Error = ErrorMessage(Format(
                            "Shader parameter '{}' has a null storage render target", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkTarget = static_cast<const VulkanRenderTarget&>(**Target);
                    VulkanTransitionImage(Buf,
                                    LocalStates,
                                    VkTarget.GetVkImage(),
                                    ShaderStage,
                                    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                                    vk::ImageLayout::eGeneral,
                                    false,
                                    vk::ImageAspectFlagBits::eColor);
                    if (bUpdateDescriptors) {
                        auto& ResourceBindings = (*Instance)->ResourceBindings;
                        if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *Target) {
                            Descriptors->WriteStorageImageDescriptor(
                                *(*Instance)->Set, Binding.Binding, VkTarget.GetVkImageView());
                            ResourceBindings[Binding.Binding] = *Target;
                        }
                    }
                    continue;
                }

                if (!bUpdateDescriptors)
                    continue;

                if (const auto* GeometryTable = std::get_if<RHIRayTracingGeometryTable*>(&Value)) {
                    if (!*GeometryTable) {
                        Error = ErrorMessage(Format(
                            "Shader parameter '{}' has a null BDA geometry table", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkTable = static_cast<const VulkanBdaRayTracingGeometryTable&>(**GeometryTable);
                    if (VkTable.GetByteSize() == 0) {
                        Error = ErrorMessage(Format(
                            "Shader parameter '{}' references an empty BDA geometry table", Binding.ParameterPath));
                        return;
                    }
                    auto& ResourceBindings = (*Instance)->ResourceBindings;
                    if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *GeometryTable) {
                        Descriptors->WriteStorageBufferDescriptor(
                            *(*Instance)->Set, Binding.Binding, VkTable.GetVkBuffer(), VkTable.GetCapacity());
                        ResourceBindings[Binding.Binding] = *GeometryTable;
                    }
                    continue;
                }

                if (const auto* VertexBufferPtr = std::get_if<RHIVertexBuffer*>(&Value)) {
                    if (!*VertexBufferPtr) {
                        Error = ErrorMessage(Format(
                            "Shader parameter '{}' has a null storage vertex buffer", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkBuffer = static_cast<const VulkanVertexBuffer&>(**VertexBufferPtr);
                    auto& ResourceBindings = (*Instance)->ResourceBindings;
                    if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *VertexBufferPtr) {
                        Descriptors->WriteStorageBufferDescriptor(
                            *(*Instance)->Set,
                            Binding.Binding,
                            VkBuffer.GetVkBuffer(),
                            VkBuffer.GetStride() * VkBuffer.GetVertexCount());
                        ResourceBindings[Binding.Binding] = *VertexBufferPtr;
                    }
                    continue;
                }

                if (const auto* IndexBufferPtr = std::get_if<RHIIndexBuffer*>(&Value)) {
                    if (!*IndexBufferPtr) {
                        Error = ErrorMessage(Format(
                            "Shader parameter '{}' has a null storage index buffer", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkBuffer = static_cast<const VulkanIndexBuffer&>(**IndexBufferPtr);
                    auto& ResourceBindings = (*Instance)->ResourceBindings;
                    if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *IndexBufferPtr) {
                        Descriptors->WriteStorageBufferDescriptor(
                            *(*Instance)->Set, Binding.Binding, VkBuffer.GetVkBuffer(), VkBuffer.GetIndexCount() * sizeof(Uint32));
                        ResourceBindings[Binding.Binding] = *IndexBufferPtr;
                    }
                    continue;
                }

                if (const auto* Texture = std::get_if<RHISampledTexture*>(&Value)) {
                    if (!*Texture) {
                        Error = ErrorMessage(Format(
                            "Shader parameter '{}' has a null sampled texture", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkTexture = static_cast<const VulkanSampledTexture&>(**Texture);
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

                if (const auto* Array = std::get_if<RHIResourceArray<RHISampledTexture>>(&Value)) {
                    if (Array->GetSize() == 0) {
                        Error = ErrorMessage(Format(
                            "Shader parameter '{}' has an empty sampled-texture resource array",
                            Binding.ParameterPath));
                        return;
                    }
                    const bool bRuntimeArray = Binding.ArrayCount == std::numeric_limits<Uint32>::max();
                    if (!bRuntimeArray && Array->GetSize() != Binding.ArrayCount) {
                        Error = ErrorMessage(Format(
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
                            Error = ErrorMessage(Format(
                                "Fixed-size shader parameter '{}' has an unset sampled-texture array slot {}",
                                Binding.ParameterPath,
                                Slot));
                            return;
                        }
                        const auto& VkTexture = static_cast<const VulkanSampledTexture&>(*Texture);
                        Descriptors->WriteSampledTextureDescriptor(*(*Instance)->Set,
                                                                   Binding.Binding,
                                                                   Slot,
                                                                   VkTexture.GetVkImageView(),
                                                                   vk::ImageLayout::eShaderReadOnlyOptimal);
                    }
                    continue;
                }

                if (const auto* TlasPtr = std::get_if<RHITopLevelAccelerationStructure*>(&Value)) {
                    if (!*TlasPtr) {
                        Error = ErrorMessage(Format(
                            "Shader parameter '{}' has a null top-level acceleration structure", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkTlas = static_cast<const VulkanTopLevelAccelerationStructure&>(**TlasPtr);
                    auto& ResourceBindings = (*Instance)->ResourceBindings;
                    if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *TlasPtr) {
                        Descriptors->WriteAccelerationStructureDescriptor(
                            *(*Instance)->Set, Binding.Binding, VkTlas.GetAccelerationStructure());
                        ResourceBindings[Binding.Binding] = *TlasPtr;
                    }
                    continue;
                }

                if (const auto* SamplerPtr = std::get_if<RHISampler*>(&Value)) {
                    if (!*SamplerPtr) {
                        Error = ErrorMessage(Format(
                            "Shader parameter '{}' has a null sampler", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkSampler = static_cast<const VulkanSampler&>(**SamplerPtr);
                    auto& ResourceBindings = (*Instance)->ResourceBindings;
                    if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *SamplerPtr) {
                        Descriptors->WriteSamplerDescriptor(
                            *(*Instance)->Set, Binding.Binding, VkSampler.GetVkSampler());
                        ResourceBindings[Binding.Binding] = *SamplerPtr;
                    }
                    continue;
                }

                Error = ErrorMessage(Format("Shader parameter '{}' is unset", Binding.ParameterPath));
                return;
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

            Buf.bindDescriptorSets(BindPoint,
                                   Pipeline.GetPipelineLayout(),
                                   SetIndex,
                                   std::array{*(*Instance)->Set},
                                   DynamicOffsets);
        }
    }

    auto operator()(const RHISetGraphicsPipelineCmd& Cmd) -> void {
        if (!Cmd.PipelinePtr)
            return;

        auto& Pipeline = static_cast<VulkanGraphicsPipeline&>(*Cmd.PipelinePtr);
        Buf.bindPipeline(vk::PipelineBindPoint::eGraphics, Pipeline.Get());
        m_BoundPipeline     = Cmd.PipelinePtr;
        m_BoundPipelineType = BoundPipelineType::Graphics;
    }

    auto operator()(const RHISetRayTracingPipelineCmd& Cmd) -> void {
        if (!Cmd.PipelinePtr)
            return;
        auto& Pipeline = static_cast<VulkanRayTracingPipeline&>(*Cmd.PipelinePtr);
        Buf.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, Pipeline.Get());
        m_BoundPipeline     = Cmd.PipelinePtr;
        m_BoundPipelineType = BoundPipelineType::RayTracing;
    }

    auto operator()(const RHIUpdateRayTracingGeometryTableCmd& Cmd) -> void {
        if (!Cmd.TablePtr) {
            Error = ErrorMessage("BDA geometry table update has no target table");
            return;
        }
        auto& Table = static_cast<VulkanBdaRayTracingGeometryTable&>(*Cmd.TablePtr);
        if (auto Result = Table.Update(Cmd.Update); !Result) {
            Error = Result.error().Append("BDA geometry table update failed");
            return;
        }
        const vk::MemoryBarrier2 Barrier{
            .srcStageMask  = vk::PipelineStageFlagBits2::eHost,
            .srcAccessMask = vk::AccessFlagBits2::eHostWrite,
            .dstStageMask  = vk::PipelineStageFlagBits2::eRayTracingShaderKHR,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        };
        const vk::DependencyInfo Dependency{.memoryBarrierCount = 1, .pMemoryBarriers = &Barrier};
        Buf.pipelineBarrier2(Dependency);
    }

    auto operator()(const RHIBuildOrUpdateTopLevelAccelerationStructureCmd& Cmd) -> void {
        if (!Cmd.TargetPtr)
            return;
        auto& Tlas = static_cast<VulkanTopLevelAccelerationStructure&>(*Cmd.TargetPtr);
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
        const vk::DependencyInfo Dependency{
            .memoryBarrierCount = 1,
            .pMemoryBarriers    = &BuildBarrier,
        };
        Buf.pipelineBarrier2(Dependency);
    }

    auto operator()(const RHITraceRaysCmd& Cmd) -> void {
        if (!Cmd.PipelinePtr)
            return;
        auto& Pipeline = static_cast<VulkanRayTracingPipeline&>(*Cmd.PipelinePtr);
        Buf.traceRaysKHR(Pipeline.GetRayGenerationRegion(),
                         Pipeline.GetMissRegion(),
                         Pipeline.GetHitRegion(),
                         Pipeline.GetCallableRegion(),
                         Cmd.Width,
                         Cmd.Height,
                         Cmd.Depth);

        const vk::MemoryBarrier2 TraceBarrier{
            .srcStageMask  = vk::PipelineStageFlagBits2::eRayTracingShaderKHR,
            .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR,
            .dstStageMask  = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
            .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR,
        };
        const vk::DependencyInfo Dependency{
            .memoryBarrierCount = 1,
            .pMemoryBarriers    = &TraceBarrier,
        };
        Buf.pipelineBarrier2(Dependency);
    }

    auto operator()(const RHIPushConstantsCmd& Cmd) -> void {
        if (!Cmd.PipelinePtr || Cmd.Data.empty())
            return;
        if (m_BoundPipeline != Cmd.PipelinePtr) {
            Error = ErrorMessage("PushConstants pipeline is not currently bound");
            return;
        }

        vk::PipelineLayout PipelineLayout = nullptr;
        vk::ShaderStageFlags Stages = {};
        Uint32 PushConstantSize = 0;
        switch (m_BoundPipelineType) {
        case BoundPipelineType::Graphics: {
            const auto& Pipeline = static_cast<const VulkanGraphicsPipeline&>(*Cmd.PipelinePtr);
            PipelineLayout = Pipeline.GetPipelineLayout();
            Stages = vk::ShaderStageFlagBits::eAllGraphics;
            PushConstantSize = Pipeline.GetPushConstantSize();
            break;
        }
        case BoundPipelineType::RayTracing: {
            const auto& Pipeline = static_cast<const VulkanRayTracingPipeline&>(*Cmd.PipelinePtr);
            PipelineLayout = Pipeline.GetPipelineLayout();
            Stages = Pipeline.GetPushConstantStages();
            PushConstantSize = Pipeline.GetPushConstantSize();
            break;
        }
        case BoundPipelineType::Unknown:
            Error = ErrorMessage("PushConstants has no bound pipeline type");
            return;
        }

        if (Cmd.Offset + Cmd.Data.size() > PushConstantSize) {
            Error = ErrorMessage(Format("PushConstants range exceeds reflected pipeline push-constant size ({} + {} > {})",
                                              Cmd.Offset,
                                              Cmd.Data.size(),
                                              PushConstantSize));
            return;
        }

        Buf.pushConstants(PipelineLayout,
                          Stages,
                          Cmd.Offset,
                          static_cast<Uint32>(Cmd.Data.size()),
                          Cmd.Data.data());
    }

    auto operator()(const RHIBindShaderParametersCmd& Cmd) -> void {
        if (!Cmd.PipelinePtr)
            return;
        if (!Descriptors || !ConstantArena || !DrawConstantArena) {
            Error = ErrorMessage("VulkanCommandVisitor: descriptor manager or constant arena is missing");
            return;
        }
        if (m_BoundPipeline != Cmd.PipelinePtr) {
            Error = ErrorMessage("BindShaderParameters pipeline is not currently bound");
            return;
        }

        switch (m_BoundPipelineType) {
        case BoundPipelineType::Graphics:
            BindShaderParameters(Cmd,
                                 static_cast<VulkanGraphicsPipeline&>(*Cmd.PipelinePtr),
                                 vk::PipelineBindPoint::eGraphics,
                                 vk::PipelineStageFlagBits2::eAllGraphics);
            return;
        case BoundPipelineType::RayTracing:
            BindShaderParameters(Cmd,
                                 static_cast<VulkanRayTracingPipeline&>(*Cmd.PipelinePtr),
                                 vk::PipelineBindPoint::eRayTracingKHR,
                                 vk::PipelineStageFlagBits2::eRayTracingShaderKHR);
            return;
        case BoundPipelineType::Unknown:
            Error = ErrorMessage("BindShaderParameters has no bound pipeline type");
            return;
        }
    }

    auto operator()(const RHISetViewportCmd& Cmd) -> void {
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

    auto operator()(const RHISetFullViewportCmd& Cmd) -> void {
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

    auto operator()(const RHISetScissorCmd& Cmd) -> void {
        Buf.setScissor(0, {vk::Rect2D{{Cmd.X, Cmd.Y}, {Cmd.Width, Cmd.Height}}});
    }

    auto operator()(const RHISetFullScissorRectCmd& /*Cmd*/) -> void {
        Buf.setScissor(0, {vk::Rect2D{{0, 0}, CurrentRenderExtent}});
    }

    auto operator()(const RHIDrawIndexedCmd& Cmd) -> void {
        if (!Cmd.PipelinePtr || !Cmd.VertexBuffers[0] || !Cmd.IndexBufferPtr)
            return;

        const auto& VkIB   = static_cast<const VulkanIndexBuffer&>(*Cmd.IndexBufferPtr);
        for (Uint32 Binding = 0; Binding < Cmd.VertexBuffers.size(); ++Binding) {
            auto* VertexBufferPtr = Cmd.VertexBuffers[Binding];
            if (!VertexBufferPtr)
                continue;
            const auto& VkVB = static_cast<const VulkanVertexBuffer&>(*VertexBufferPtr);
            Buf.bindVertexBuffers(Binding, {VkVB.GetVkBuffer()}, {0});
        }
        Buf.bindIndexBuffer(VkIB.GetVkBuffer(), 0, vk::IndexType::eUint32);
        Buf.drawIndexed(static_cast<Uint32>(VkIB.GetIndexCount()), 1, 0, 0, 0);
    }

    auto operator()(const RHIDrawCmd& Cmd) -> void {
        if (!Cmd.PipelinePtr || !Cmd.VertexBuffers[0])
            return;

        for (Uint32 Binding = 0; Binding < Cmd.VertexBuffers.size(); ++Binding) {
            auto* VertexBufferPtr = Cmd.VertexBuffers[Binding];
            if (!VertexBufferPtr)
                continue;
            const auto& VkVB = static_cast<const VulkanVertexBuffer&>(*VertexBufferPtr);
            Buf.bindVertexBuffers(Binding, {VkVB.GetVkBuffer()}, {0});
        }
        const auto& VkVB = static_cast<const VulkanVertexBuffer&>(*Cmd.VertexBuffers[0]);
        Buf.draw(static_cast<Uint32>(VkVB.GetVertexCount()), 1, 0, 0);
    }
};

} // namespace SoulEngine
