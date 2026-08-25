export module Vulkan:Command;

import Core;
import RHI;
import vulkan;
import std;

import :Types;
import :Capability;
import :Swapchain;
import :Buffer;
import :Pipeline;
import :RayTracingPipeline;
import :Sampler;
import :Texture;
import :Descriptor;
import :AccelerationStructure;

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

/// Callable for std::visit over RHICommand variants.
struct VulkanCommandVisitor final {
    vk::raii::CommandBuffer&                         Buf;
    std::unordered_map<vk::Image, VulkanImageState>& LocalStates;
    VulkanDescriptorManager*                         Descriptors                                             = nullptr;
    VulkanTransientUniformArena*                     TransientUniformArena                                   = nullptr;
    VulkanTransientShaderStorageArena*               TransientShaderStorageArena                             = nullptr;
    std::vector<std::function<void()>>* RetiredPayloads     = nullptr;
    Uint32                              FrameIndex          = 0;
    vk::Extent2D                        CurrentRenderExtent = {1, 1};
    std::vector<RHIFormat>              CurrentColorFormats = {};
    RHIFormat                           CurrentDepthFormat  = RHIFormat::Unknown;
    enum class BoundPipelineType : Uint8 {
        Unknown = 0,
        Graphics,
        RayTracing,
    };

    std::optional<ErrorMessage> Error                     = std::nullopt;
    RHIPipeline*                m_BoundPipeline           = nullptr;
    BoundPipelineType           m_BoundPipelineType       = BoundPipelineType::Unknown;
    VulkanCommandVisitor(
        vk::raii::CommandBuffer&                                                        InBuffer,
        std::unordered_map<vk::Image, VulkanImageState>&                                InLocalStates,
        VulkanDescriptorManager*                                                        InDescriptors,
        VulkanTransientUniformArena*                                                    InTransientUniformArena,
        VulkanTransientShaderStorageArena*                                              InTransientShaderStorageArena,
        std::vector<std::function<void()>>* InRetiredPayloads,
        Uint32                              InFrameIndex)
        : Buf(InBuffer),
          LocalStates(InLocalStates),
          Descriptors(InDescriptors),
          TransientUniformArena(InTransientUniformArena),
          TransientShaderStorageArena(InTransientShaderStorageArena),
          RetiredPayloads(InRetiredPayloads),
          FrameIndex(InFrameIndex) {}

    /// Begin rendering scope from RHIPass desc.
    auto BeginPass(const RHIRenderingDesc& Desc) -> void {
        // ── Resolve color attachment ──────────────────────────────────
        std::vector<vk::RenderingAttachmentInfo> ColorAttachments = {};
        ColorAttachments.reserve(Desc.ColorAttachments.size());
        CurrentColorFormats.clear();
        CurrentColorFormats.reserve(Desc.ColorAttachments.size());
        CurrentDepthFormat  = RHIFormat::Unknown;
        Uint32 RenderWidth  = 0;
        Uint32 RenderHeight = 0;

        for (const auto& AttachmentDesc : Desc.ColorAttachments) {
            auto* ColorTarget = AttachmentDesc.TextureRef.TryGet();
            if (!ColorTarget) {
                Error = ErrorMessage("Rendering pass color attachment is not ready");
                return;
            }
            const auto& ColorRT = static_cast<const VulkanRenderTarget&>(*ColorTarget);
            VulkanTransitionImage(Buf,
                                  LocalStates,
                                  ColorRT.GetVkImage(),
                                  vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                  // Clear uses eClear, which does not load old contents; eLoad must
                                  // read the existing color while also writing the attachment.
                                  AttachmentDesc.Clear ? vk::AccessFlagBits2::eColorAttachmentWrite
                                                       : vk::AccessFlagBits2::eColorAttachmentRead |
                                                             vk::AccessFlagBits2::eColorAttachmentWrite,
                                  vk::ImageLayout::eColorAttachmentOptimal,
                                  true,
                                  ToVkImageAspect(ColorRT.GetFormat()));
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
            CurrentColorFormats.push_back(ColorRT.GetFormat());
            RenderWidth  = RenderWidth == 0 ? ColorRT.GetWidth() : (std::min)(RenderWidth, ColorRT.GetWidth());
            RenderHeight = RenderHeight == 0 ? ColorRT.GetHeight() : (std::min)(RenderHeight, ColorRT.GetHeight());
        }
        CurrentRenderExtent = vk::Extent2D{RenderWidth, RenderHeight};

        // ── Resolve depth attachment (optional) ───────────────────────
        std::optional<vk::RenderingAttachmentInfo> DepthAttachment = std::nullopt;
        if (Desc.DepthAttachment.has_value()) {
            auto* DepthTarget = Desc.DepthAttachment->TextureRef.TryGet();
            if (!DepthTarget) {
                Error = ErrorMessage("Rendering pass depth attachment is not ready");
                return;
            }
            auto& VkDepthRT    = static_cast<const VulkanRenderTarget&>(*DepthTarget);
            CurrentDepthFormat = VkDepthRT.GetFormat();
            auto DepthImage    = VkDepthRT.GetVkImage();
            auto DepthView     = VkDepthRT.GetVkImageView();
            auto DepthAspect   = ToVkImageAspect(VkDepthRT.GetFormat());
            VulkanTransitionImage(Buf,
                                  LocalStates,
                                  DepthImage,
                                  // Depth attachment writes, including the final store operation, may
                                  // occur in either fragment-test stage. This is synchronization
                                  // coverage, not a request to force Early-Z or Late-Z execution.
                                  vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                      vk::PipelineStageFlagBits2::eLateFragmentTests,
                                  // Depth follows the same clear/load distinction; depth eLoad reads
                                  // the existing depth, so the load path needs read and write access.
                                  Desc.DepthAttachment->Clear ? vk::AccessFlagBits2::eDepthStencilAttachmentWrite
                                                              : vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                                                    vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
                                  vk::ImageLayout::eDepthAttachmentOptimal,
                                  true,
                                  DepthAspect);
            DepthAttachment = vk::RenderingAttachmentInfo{
                .imageView   = DepthView,
                .imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
                .loadOp      = Desc.DepthAttachment->Clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
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
        if (Desc.DepthAttachment.has_value()) {
            auto* DepthTarget = Desc.DepthAttachment->TextureRef.TryGet();
            if (!DepthTarget)
                return;
            auto& VkDepthRT            = static_cast<const VulkanRenderTarget&>(*DepthTarget);
            CurrentRenderExtent.width  = (std::min)(CurrentRenderExtent.width, VkDepthRT.GetWidth());
            CurrentRenderExtent.height = (std::min)(CurrentRenderExtent.height, VkDepthRT.GetHeight());
        }

        // ── Begin dynamic rendering ───────────────────────────────────
        vk::RenderingInfo RenderingInfo{
            .renderArea           = vk::Rect2D{{0, 0}, CurrentRenderExtent},
            .layerCount           = 1,
            .colorAttachmentCount = static_cast<Uint32>(ColorAttachments.size()),
            .pColorAttachments    = ColorAttachments.data(),
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
                              PipelineType&                     Pipeline,
                              vk::PipelineBindPoint             BindPoint,
                              vk::PipelineStageFlags2           ShaderStage) -> void {
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
                    Error = ErrorMessage(
                        Format("Runtime resource array '{}' uses unsupported resource type", Binding.ParameterPath));
                    return;
                }
                const auto* Array = std::get_if<RHIResourceArray<RHISampledTexture>>(&Values[Index]);
                if (!Array || Array->GetSize() == 0) {
                    Error =
                        ErrorMessage(Format("Runtime sampled texture array '{}' is missing a non-empty resource array",
                                            Binding.ParameterPath));
                    return;
                }
                VariableDescriptorCount = Array->GetSize();
            }

            auto Instance = Pipeline.GetOrCreateDescriptorSetInstance(
                Cmd.Parameters.GetId(), FrameIndex, SetIndex, VariableDescriptorCount, *Descriptors);
            if (!Instance) {
                Error = Instance.error();
                return;
            }

            const bool bUpdateDescriptors =
                !(*Instance)->Initialized || (*Instance)->ParameterRevision != Parameters.GetRevision();
            std::vector<std::pair<Uint32, Uint32>> DynamicOffsetWrites;
            for (Uint32 Index = 0; Index < Bindings.size(); ++Index) {
                const auto& Binding = Bindings[Index];
                const auto& Value   = Values[Index];

                if (const auto* TransientConstantRef =
                        std::get_if<RHIRef<RHITransientConstantBuffer>>(&Value)) {
                    const auto* TransientConstant = TransientConstantRef->TryGet();
                    if (!TransientConstant) {
                        Error = ErrorMessage(
                            Format("Shader parameter '{}' references an invalid transient constant buffer",
                                   Binding.ParameterPath));
                        return;
                    }
                    const auto& VulkanBuffer =
                        static_cast<const VulkanTransientConstantBuffer&>(*TransientConstant);
                    if (bUpdateDescriptors) {
                        Descriptors->WriteConstantDescriptor(
                            *(*Instance)->Set,
                            Binding.Binding,
                            TransientUniformArena->GetVkBuffer(),
                            VulkanBuffer.GetSize());
                    }
                    DynamicOffsetWrites.push_back({Binding.Binding, VulkanBuffer.GetOffset()});
                    continue;
                }

                if (const auto* Target = std::get_if<RHIRenderTarget*>(&Value)) {
                    if (!*Target) {
                        Error = ErrorMessage(
                            Format("Shader parameter '{}' has a null render target", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkTarget  = static_cast<const VulkanRenderTarget&>(**Target);
                    const bool  IsSampled = Binding.Type == ShaderResourceType::SampledTexture;
                    VulkanTransitionImage(
                        Buf,
                        LocalStates,
                        VkTarget.GetVkImage(),
                        ShaderStage,
                        IsSampled ? vk::AccessFlagBits2::eShaderRead
                                  : vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        IsSampled ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eGeneral,
                        false,
                        ToVkImageAspect(VkTarget.GetFormat()));
                    if (bUpdateDescriptors) {
                        auto& ResourceBindings = (*Instance)->ResourceBindings;
                        if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *Target) {
                            if (IsSampled)
                                Descriptors->WriteSampledTextureDescriptor(*(*Instance)->Set,
                                                                           Binding.Binding,
                                                                           0,
                                                                           VkTarget.GetVkImageView(),
                                                                           vk::ImageLayout::eShaderReadOnlyOptimal);
                            else
                                Descriptors->WriteStorageImageDescriptor(
                                    *(*Instance)->Set, Binding.Binding, VkTarget.GetVkImageView());
                            ResourceBindings[Binding.Binding] = *Target;
                        }
                    }
                    continue;
                }

                if (!bUpdateDescriptors)
                    continue;

                if (const auto* TransientStorageRef =
                        std::get_if<RHIRef<RHITransientShaderStorageBuffer>>(&Value)) {
                    const auto* TransientStorage = TransientStorageRef->TryGet();
                    if (!TransientStorage) {
                        Error = ErrorMessage(
                            Format("Shader parameter '{}' references an invalid transient shader storage buffer",
                                   Binding.ParameterPath));
                        return;
                    }
                    const auto& VulkanBuffer =
                        static_cast<const VulkanTransientShaderStorageBuffer&>(*TransientStorage);
                    Descriptors->WriteStorageBufferDescriptor(*(*Instance)->Set,
                                                              Binding.Binding,
                                                              TransientShaderStorageArena->GetVkBuffer(),
                                                              VulkanBuffer.GetSize(),
                                                              VulkanBuffer.GetOffset());
                    continue;
                }

                if (const auto* VertexBuffer = std::get_if<RHIVertexBuffer*>(&Value)) {
                    if (!*VertexBuffer) {
                        Error = ErrorMessage(
                            Format("Shader parameter '{}' has a null storage vertex buffer", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkBuffer         = static_cast<const VulkanVertexBuffer&>(**VertexBuffer);
                    auto&       ResourceBindings = (*Instance)->ResourceBindings;
                    if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *VertexBuffer) {
                        Descriptors->WriteStorageBufferDescriptor(*(*Instance)->Set,
                                                                  Binding.Binding,
                                                                  VkBuffer.GetVkBuffer(),
                                                                  VkBuffer.GetStride() * VkBuffer.GetVertexCount());
                        ResourceBindings[Binding.Binding] = *VertexBuffer;
                    }
                    continue;
                }

                if (const auto* IndexBuffer = std::get_if<RHIIndexBuffer*>(&Value)) {
                    if (!*IndexBuffer) {
                        Error = ErrorMessage(
                            Format("Shader parameter '{}' has a null storage index buffer", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkBuffer         = static_cast<const VulkanIndexBuffer&>(**IndexBuffer);
                    auto&       ResourceBindings = (*Instance)->ResourceBindings;
                    if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *IndexBuffer) {
                        Descriptors->WriteStorageBufferDescriptor(*(*Instance)->Set,
                                                                  Binding.Binding,
                                                                  VkBuffer.GetVkBuffer(),
                                                                  VkBuffer.GetIndexCount() * sizeof(Uint32));
                        ResourceBindings[Binding.Binding] = *IndexBuffer;
                    }
                    continue;
                }

                if (const auto* Texture = std::get_if<RHISampledTexture*>(&Value)) {
                    if (!*Texture) {
                        Error = ErrorMessage(
                            Format("Shader parameter '{}' has a null sampled texture", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkTexture        = static_cast<const VulkanSampledTexture&>(**Texture);
                    auto&       ResourceBindings = (*Instance)->ResourceBindings;
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
                        Error = ErrorMessage(Format("Shader parameter '{}' has an empty sampled-texture resource array",
                                                    Binding.ParameterPath));
                        return;
                    }
                    const bool bRuntimeArray = Binding.ArrayCount == std::numeric_limits<Uint32>::max();
                    if (!bRuntimeArray && Array->GetSize() != Binding.ArrayCount) {
                        Error = ErrorMessage(Format("Shader parameter '{}' sampled-texture array size {} does not "
                                                    "match reflected array count {}",
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
                            Error = ErrorMessage(
                                Format("Fixed-size shader parameter '{}' has an unset sampled-texture array slot {}",
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

                if (const auto* RHIAccelerationStructure = std::get_if<RHITopLevelAccelerationStructure*>(&Value)) {
                    if (!*RHIAccelerationStructure) {
                        Error = ErrorMessage(Format("Shader parameter '{}' has a null top-level acceleration structure",
                                                    Binding.ParameterPath));
                        return;
                    }
                    const auto& VkTlas =
                        static_cast<const VulkanTopLevelAccelerationStructure&>(**RHIAccelerationStructure);
                    auto& ResourceBindings = (*Instance)->ResourceBindings;
                    if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *RHIAccelerationStructure) {
                        Descriptors->WriteAccelerationStructureDescriptor(
                            *(*Instance)->Set, Binding.Binding, VkTlas.GetAccelerationStructure());
                        ResourceBindings[Binding.Binding] = *RHIAccelerationStructure;
                    }
                    continue;
                }

                if (const auto* Sampler = std::get_if<RHISampler*>(&Value)) {
                    if (!*Sampler) {
                        Error = ErrorMessage(Format("Shader parameter '{}' has a null sampler", Binding.ParameterPath));
                        return;
                    }
                    const auto& VkSampler        = static_cast<const VulkanSampler&>(**Sampler);
                    auto&       ResourceBindings = (*Instance)->ResourceBindings;
                    if (!(*Instance)->Initialized || ResourceBindings[Binding.Binding] != *Sampler) {
                        Descriptors->WriteSamplerDescriptor(
                            *(*Instance)->Set, Binding.Binding, VkSampler.GetVkSampler());
                        ResourceBindings[Binding.Binding] = *Sampler;
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

            Buf.bindDescriptorSets(
                BindPoint, Pipeline.GetPipelineLayout(), SetIndex, std::array{*(*Instance)->Set}, DynamicOffsets);
        }
    }

    auto operator()(const RHISetGraphicsPipelineCmd& Cmd) -> void {
        auto* PipelinePtr = Cmd.PipelineRef.TryGet();
        if (!PipelinePtr)
            return;

        auto& Pipeline = static_cast<VulkanGraphicsPipeline&>(*PipelinePtr);
        if (!CurrentColorFormats.empty() && !Pipeline.IsCompatibleWith(CurrentColorFormats, CurrentDepthFormat)) {
            Error = ErrorMessage("Graphics pipeline attachment formats do not match the active rendering pass");
            return;
        }
        Buf.bindPipeline(vk::PipelineBindPoint::eGraphics, Pipeline.Get());
        m_BoundPipeline     = PipelinePtr;
        m_BoundPipelineType = BoundPipelineType::Graphics;
    }

    auto operator()(const RHISetRayTracingPipelineCmd& Cmd) -> void {
        auto* PipelinePtr = Cmd.PipelineRef.TryGet();
        if (!PipelinePtr)
            return;
        auto& Pipeline = static_cast<VulkanRayTracingPipeline&>(*PipelinePtr);
        Buf.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, Pipeline.Get());
        m_BoundPipeline     = PipelinePtr;
        m_BoundPipelineType = BoundPipelineType::RayTracing;
    }

    auto operator()(const RHIBuildOrUpdateTopLevelAccelerationStructureCmd& Cmd) -> void {
        auto* TargetPtr = Cmd.TargetRef.TryGet();
        if (!TargetPtr)
            return;
        auto& Tlas = static_cast<VulkanTopLevelAccelerationStructure&>(*TargetPtr);
        if (auto R = Tlas.RecordBuild(Buf, Cmd.Instances, Cmd.Mode, RetiredPayloads); !R) {
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
        auto* PipelinePtr = Cmd.PipelineRef.TryGet();
        if (!PipelinePtr)
            return;
        auto& Pipeline = static_cast<VulkanRayTracingPipeline&>(*PipelinePtr);
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
        RHIPipeline* PipelinePtr = Cmd.PipelineRef.Graphics.TryGet();
        if (!PipelinePtr)
            PipelinePtr = Cmd.PipelineRef.RayTracing.TryGet();
        if (!PipelinePtr || Cmd.Data.empty())
            return;
        if (m_BoundPipeline != PipelinePtr) {
            Error = ErrorMessage("PushConstants pipeline is not currently bound");
            return;
        }

        vk::PipelineLayout   PipelineLayout   = nullptr;
        vk::ShaderStageFlags Stages           = {};
        Uint32               PushConstantSize = 0;
        switch (m_BoundPipelineType) {
        case BoundPipelineType::Graphics: {
            const auto& Pipeline = static_cast<const VulkanGraphicsPipeline&>(*PipelinePtr);
            PipelineLayout       = Pipeline.GetPipelineLayout();
            Stages               = vk::ShaderStageFlagBits::eAllGraphics;
            PushConstantSize     = Pipeline.GetPushConstantSize();
            break;
        }
        case BoundPipelineType::RayTracing: {
            const auto& Pipeline = static_cast<const VulkanRayTracingPipeline&>(*PipelinePtr);
            PipelineLayout       = Pipeline.GetPipelineLayout();
            Stages               = Pipeline.GetPushConstantStages();
            PushConstantSize     = Pipeline.GetPushConstantSize();
            break;
        }
        case BoundPipelineType::Unknown:
            Error = ErrorMessage("PushConstants has no bound pipeline type");
            return;
        }

        if (Cmd.Offset + Cmd.Data.size() > PushConstantSize) {
            Error =
                ErrorMessage(Format("PushConstants range exceeds reflected pipeline push-constant size ({} + {} > {})",
                                    Cmd.Offset,
                                    Cmd.Data.size(),
                                    PushConstantSize));
            return;
        }

        Buf.pushConstants(PipelineLayout, Stages, Cmd.Offset, static_cast<Uint32>(Cmd.Data.size()), Cmd.Data.data());
    }

    auto operator()(const RHIBindShaderParametersCmd& Cmd) -> void {
        RHIPipeline* PipelinePtr = Cmd.PipelineRef.Graphics.TryGet();
        if (!PipelinePtr)
            PipelinePtr = Cmd.PipelineRef.RayTracing.TryGet();
        if (!PipelinePtr)
            return;
        if (!Descriptors || !TransientUniformArena || !TransientShaderStorageArena) {
            Error = ErrorMessage("VulkanCommandVisitor: descriptor manager or transient arena is missing");
            return;
        }
        if (m_BoundPipeline != PipelinePtr) {
            Error = ErrorMessage("BindShaderParameters pipeline is not currently bound");
            return;
        }

        switch (m_BoundPipelineType) {
        case BoundPipelineType::Graphics:
            BindShaderParameters(Cmd,
                                 static_cast<VulkanGraphicsPipeline&>(*PipelinePtr),
                                 vk::PipelineBindPoint::eGraphics,
                                 vk::PipelineStageFlagBits2::eAllGraphics);
            return;
        case BoundPipelineType::RayTracing:
            BindShaderParameters(Cmd,
                                 static_cast<VulkanRayTracingPipeline&>(*PipelinePtr),
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
        if (!Cmd.PipelineRef.TryGet() || !Cmd.VertexBufferRefs[0].TryGet() || !Cmd.IndexBufferRef.TryGet())
            return;

        for (Uint32 Binding = 0; Binding < Cmd.VertexBufferRefs.size(); ++Binding) {
            auto* VertexBufferPtr = Cmd.VertexBufferRefs[Binding].TryGet();
            if (!VertexBufferPtr)
                continue;
            const auto& VkVB = static_cast<const VulkanVertexBuffer&>(*VertexBufferPtr);
            Buf.bindVertexBuffers(Binding, {VkVB.GetVkBuffer()}, {0});
        }

        const auto& VkIB = static_cast<const VulkanIndexBuffer&>(*Cmd.IndexBufferRef.TryGet());
        Buf.bindIndexBuffer(VkIB.GetVkBuffer(), 0, vk::IndexType::eUint32);
        Buf.drawIndexed(static_cast<Uint32>(VkIB.GetIndexCount()), 1, 0, 0, 0);
    }

    auto operator()(const RHIDrawCmd& Cmd) -> void {
        if (!Cmd.PipelineRef.TryGet())
            return;
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
        if (!Cmd.PipelineRef.TryGet() || !TransientShaderStorageArena)
            return;
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
        Buf.drawIndirect(
            TransientShaderStorageArena->GetVkBuffer(),
            VulkanBuffer.GetOffset() + Cmd.Offset,
            Cmd.DrawCount,
            Cmd.Stride);
    }
};

} // namespace SoulEngine
