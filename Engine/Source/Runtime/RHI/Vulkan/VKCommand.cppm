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

using namespace SoulEngine::Core;

namespace SoulEngine::RHI::Vulkan {

/// Callable for std::visit over RHI::Command variants.
struct CommandVisitor {
    vk::raii::CommandBuffer&                   Buf;
    std::unordered_map<vk::Image, ImageState>& LocalStates;
    DescriptorManager*                         Descriptors                = nullptr;
    UniformBufferArena*                        ConstantArena              = nullptr;
    TransientUniformBufferArena*               DrawConstantArena          = nullptr;
    Uint32                                     FrameIndex                 = 0;
    vk::Extent2D                               CurrentRenderExtent        = {1, 1};
    enum class BoundPipelineType : Uint8 {
        Unknown = 0,
        Graphics,
        RayTracing,
    };

    std::optional<ErrorMessage>                Error                      = std::nullopt;
    RHI::Pipeline*                             m_BoundPipeline            = nullptr;
    BoundPipelineType                          m_BoundPipelineType        = BoundPipelineType::Unknown;

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

    template <typename PipelineType>
    auto BindShaderParameters(const RHI::BindShaderParametersCmd& Cmd,
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

                if (const auto* Target = std::get_if<RHI::RenderTarget*>(&Value)) {
                    if (!*Target) {
                        Error = ErrorMessage(Core::Format(
                            "Shader parameter '{}' has a null storage render target", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkTarget = static_cast<const Vulkan::RenderTarget&>(**Target);
                    TransitionImage(Buf,
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

                if (const auto* GeometryTable = std::get_if<RHI::RayTracingGeometryTable*>(&Value)) {
                    if (!*GeometryTable) {
                        Error = ErrorMessage(Core::Format(
                            "Shader parameter '{}' has a null BDA geometry table", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkTable = static_cast<const Vulkan::BdaRayTracingGeometryTable&>(**GeometryTable);
                    if (VkTable.GetByteSize() == 0) {
                        Error = ErrorMessage(Core::Format(
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

                if (const auto* VertexBuffer = std::get_if<RHI::VertexBuffer*>(&Value)) {
                    if (!*VertexBuffer) {
                        Error = ErrorMessage(Core::Format(
                            "Shader parameter '{}' has a null storage vertex buffer", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkBuffer = static_cast<const Vulkan::VertexBuffer&>(**VertexBuffer);
                    auto& ResourceBindings = (*Instance)->ResourceBindings;
                    if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *VertexBuffer) {
                        Descriptors->WriteStorageBufferDescriptor(
                            *(*Instance)->Set,
                            Binding.Binding,
                            VkBuffer.GetVkBuffer(),
                            VkBuffer.GetStride() * VkBuffer.GetVertexCount());
                        ResourceBindings[Binding.Binding] = *VertexBuffer;
                    }
                    continue;
                }

                if (const auto* IndexBuffer = std::get_if<RHI::IndexBuffer*>(&Value)) {
                    if (!*IndexBuffer) {
                        Error = ErrorMessage(Core::Format(
                            "Shader parameter '{}' has a null storage index buffer", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkBuffer = static_cast<const Vulkan::IndexBuffer&>(**IndexBuffer);
                    auto& ResourceBindings = (*Instance)->ResourceBindings;
                    if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *IndexBuffer) {
                        Descriptors->WriteStorageBufferDescriptor(
                            *(*Instance)->Set, Binding.Binding, VkBuffer.GetVkBuffer(), VkBuffer.GetIndexCount() * sizeof(Uint32));
                        ResourceBindings[Binding.Binding] = *IndexBuffer;
                    }
                    continue;
                }

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

                if (const auto* AccelerationStructure = std::get_if<RHI::TopLevelAccelerationStructure*>(&Value)) {
                    if (!*AccelerationStructure) {
                        Error = ErrorMessage(Core::Format(
                            "Shader parameter '{}' has a null top-level acceleration structure", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkTlas = static_cast<const Vulkan::TopLevelAccelerationStructure&>(**AccelerationStructure);
                    auto& ResourceBindings = (*Instance)->ResourceBindings;
                    if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *AccelerationStructure) {
                        Descriptors->WriteAccelerationStructureDescriptor(
                            *(*Instance)->Set, Binding.Binding, VkTlas.GetAccelerationStructure());
                        ResourceBindings[Binding.Binding] = *AccelerationStructure;
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

                Error = ErrorMessage(Core::Format("Shader parameter '{}' is unset", Binding.ParameterPath));
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

    auto operator()(const RHI::SetGraphicsPipelineCmd& Cmd) -> void {
        if (!Cmd.PipelinePtr)
            return;

        auto& Pipeline = static_cast<Vulkan::GraphicsPipeline&>(*Cmd.PipelinePtr);
        Buf.bindPipeline(vk::PipelineBindPoint::eGraphics, Pipeline.Get());
        m_BoundPipeline     = Cmd.PipelinePtr;
        m_BoundPipelineType = BoundPipelineType::Graphics;
    }

    auto operator()(const RHI::SetRayTracingPipelineCmd& Cmd) -> void {
        if (!Cmd.PipelinePtr)
            return;
        auto& Pipeline = static_cast<Vulkan::RayTracingPipeline&>(*Cmd.PipelinePtr);
        Buf.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, Pipeline.Get());
        m_BoundPipeline     = Cmd.PipelinePtr;
        m_BoundPipelineType = BoundPipelineType::RayTracing;
    }

    auto operator()(const RHI::UpdateRayTracingGeometryTableCmd& Cmd) -> void {
        if (!Cmd.TablePtr) {
            Error = ErrorMessage("BDA geometry table update has no target table");
            return;
        }
        auto& Table = static_cast<Vulkan::BdaRayTracingGeometryTable&>(*Cmd.TablePtr);
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

    auto operator()(const RHI::BuildOrUpdateTopLevelAccelerationStructureCmd& Cmd) -> void {
        if (!Cmd.TargetPtr)
            return;
        auto& Tlas = static_cast<Vulkan::TopLevelAccelerationStructure&>(*Cmd.TargetPtr);
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

    auto operator()(const RHI::TraceRaysCmd& Cmd) -> void {
        if (!Cmd.PipelinePtr)
            return;
        auto& Pipeline = static_cast<Vulkan::RayTracingPipeline&>(*Cmd.PipelinePtr);
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

    auto operator()(const RHI::PushConstantsCmd& Cmd) -> void {
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
            const auto& Pipeline = static_cast<const Vulkan::GraphicsPipeline&>(*Cmd.PipelinePtr);
            PipelineLayout = Pipeline.GetPipelineLayout();
            Stages = vk::ShaderStageFlagBits::eAllGraphics;
            PushConstantSize = Pipeline.GetPushConstantSize();
            break;
        }
        case BoundPipelineType::RayTracing: {
            const auto& Pipeline = static_cast<const Vulkan::RayTracingPipeline&>(*Cmd.PipelinePtr);
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
            Error = ErrorMessage(Core::Format("PushConstants range exceeds reflected pipeline push-constant size ({} + {} > {})",
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

    auto operator()(const RHI::BindShaderParametersCmd& Cmd) -> void {
        if (!Cmd.PipelinePtr)
            return;
        if (!Descriptors || !ConstantArena || !DrawConstantArena) {
            Error = ErrorMessage("CommandVisitor: descriptor manager or constant arena is missing");
            return;
        }
        if (m_BoundPipeline != Cmd.PipelinePtr) {
            Error = ErrorMessage("BindShaderParameters pipeline is not currently bound");
            return;
        }

        switch (m_BoundPipelineType) {
        case BoundPipelineType::Graphics:
            BindShaderParameters(Cmd,
                                 static_cast<Vulkan::GraphicsPipeline&>(*Cmd.PipelinePtr),
                                 vk::PipelineBindPoint::eGraphics,
                                 vk::PipelineStageFlagBits2::eAllGraphics);
            return;
        case BoundPipelineType::RayTracing:
            BindShaderParameters(Cmd,
                                 static_cast<Vulkan::RayTracingPipeline&>(*Cmd.PipelinePtr),
                                 vk::PipelineBindPoint::eRayTracingKHR,
                                 vk::PipelineStageFlagBits2::eRayTracingShaderKHR);
            return;
        case BoundPipelineType::Unknown:
            Error = ErrorMessage("BindShaderParameters has no bound pipeline type");
            return;
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
